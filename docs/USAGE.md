# Parcours du prototype

Les exemples supposent que le binaire `syrnck` compilé est dans le `PATH`. Sous PowerShell, on peut aussi appeler son chemin absolu avec `&`. Les identifiants affichés par le CLI sont à réutiliser sans les chevrons des exemples.

## Stocker et reconstruire un fichier

```text
syrnck init
syrnck put Content/Map.uasset
syrnck get <identifiant-manifeste> copie.uasset
syrnck inspect manifest <identifiant-manifeste>
syrnck fsck
```

`put` épingle automatiquement `files/<identifiant-manifeste>`. Réingérer un fichier modifié affiche les octets nouveaux et réutilisés. `get` refuse d'écraser un fichier existant. Pour comparer deux versions déjà ingérées :

```text
syrnck diff <ancien-manifeste> <nouveau-manifeste>
```

Les octets réutilisables sont une mesure logique des blocs communs. Les octets effectivement transmis dépendent aussi des autres objets déjà présents sur le destinataire et des doublons au sein du fichier.

Pour compresser et, si le store est chiffré, chiffrer plusieurs blocs en parallèle :

```text
syrnck put gros-fichier.bin --workers 4 --buffer-mib 32
```

`--workers` accepte 1 à 64 ; 1 reste la valeur par défaut. `--buffer-mib` borne les octets bruts des tâches en attente de publication, à 32 Mio par défaut. La limite doit contenir au moins un bloc maximum et ne pas dépasser 1 024 Mio. Un second plafond limite les tâches à `2 × workers`, même avec de petits blocs fixes. Le tampon de lecture, les buffers encodés, les contextes des codecs et les métadonnées s'ajoutent à ce budget ; il ne s'agit pas d'un plafond sur la RAM du processus.

`stats.peak_in_flight_bytes` et `stats.peak_in_flight_blocks` donnent les maxima des tâches en attente ; ils valent zéro sur le chemin séquentiel. Les identifiants et frontières de blocs restent identiques. Les blocs déjà présents sont vérifiés et réutilisés sans recompression ; les doublons encore en attente ne sont encodés qu'une fois. Le gain dépend donc de la quantité de nouvelles données, du disque et du chiffrement.

La bibliothèque C++ reçoit ces réglages dans `IngestOptions`, troisième argument de `Store::ingest`. Son `std::stop_token` permet l'annulation entre lectures, blocs et publications. Une opération native en cours peut finir avant le retour ; les workers sont joints avant de rendre la main. Une erreur ou annulation avant la finalisation empêche la création du manifeste. `ingest` ne modifie aucune référence. Des blocs déjà écrits peuvent rester non référencés, réutilisables au retry ou supprimables par GC. Le signal d'interruption du CLI n'est pas encore relié à cette annulation coopérative. Les autres commandes utilisent encore l'ingestion séquentielle.

Le manifeste v1 reste limité à 16 Mio. L'ingestion vérifie sa taille sérialisée pendant la lecture et s'arrête avant de consommer tout un fichier trop fragmenté. Les manifestes paginés restent à développer pour dépasser cette limite.

## Cache de lectures vérifiées

Un store garde par défaut jusqu'à **64 Mio de données décodées** et **8 192 empreintes d'objets vérifiés** en mémoire. Les deux limites sont indépendantes : une donnée peut être évincée alors que son empreinte reste disponible pour un test de présence. Les objets, leurs IDs et les références de rétention gardent le format v1.

À chaque accès, le moteur relit l'enveloppe complète et calcule son empreinte BLAKE3. Il ne réutilise une validation antérieure que si les octets correspondent à une enveloppe déjà déchiffrée, décodée et vérifiée dans ce même handle. Une modification de taille ou de date n'est pas nécessaire pour invalider le cache. Le cache réduit le travail des codecs ; il ne supprime pas cette lecture disque.

L'option globale du CLI permet de régler le budget de données décodées ou de désactiver tout le cache :

```text
syrnck --cache-mib 32 put fichier.bin
syrnck --cache-mib 0 get <manifeste> nouvelle-copie.bin
```

Le budget accepte 0 à 1 024 Mio. Les structures d'index, buffers IO, codecs et pipeline s'ajoutent à ce budget. Le cache disparaît avec le processus ou le handle ; il n'est pas conservé entre deux commandes CLI séparées. Les gains les plus directs concernent les lectures répétées dans un même handle, les serveurs et les références partageant des blocs.

La bibliothèque C++ configure ces limites avec `StoreOptions`, quatrième argument du constructeur `Store`. `decoded_cache_bytes = 0` garde seulement les empreintes ; `verified_cache_entries = 0` désactive le cache d'objets. `io_stats()` expose les lectures d'enveloppes réussies, octets lus, décodages complets et hits ; `reset_io_stats()` remet ces compteurs à zéro sans vider les caches. `clear_cache()` efface les caches d'objets et de pages d'index sans supprimer d'objet ni de référence. Les buffers décodés sont nettoyés à l'éviction et à la fermeture.

`fsck` et `gc` ignorent les validations et données du cache pour vérifier les objets actuels, leurs dépendances et les hashes complets des fichiers. La validation d'une fermeture avec fichiers intègre maintenant le hash du fichier pendant sa traversée : elle ne parcourt plus tous les blocs une première fois avant de les relire pour ce hash. Un bloc répété dans un fichier reste nécessaire à chaque position dans le hash complet.

Le cache de pages d'index est indépendant : **4 Mio** par défaut, avec `--index-cache-mib 0..1024` et `StoreOptions::pack_index_cache_bytes` en C++. Le répertoire de pages et le filtre restent en mémoire lorsque ce cache est désactivé. L'ouverture et les audits vérifient tout l'index ; les pages relues sont contrôlées contre ce snapshot. Les champs `index_directory_bytes`, `index_filter_bytes`, `index_cached_bytes`, `index_cached_pages` de `io_stats()` décrivent leurs allocations utiles, hors overhead et buffers temporaires. Voir les [détails des index paginés](INDEXES.md).

## Regrouper les objets en packs

```text
syrnck pack
syrnck pack --compact
syrnck pack --max-mib 512 --max-objects 65536
syrnck gc
syrnck gc --delete
```

`pack` vérifie le store puis regroupe les nouveaux objets isolés dans des packs immuables indexés. Les packs existants restent inchangés. `--compact` regroupe explicitement tout le stock ; les plafonds de création sont 512 Mio et 65 536 objets par défaut. Les identifiants, références et enveloppes chiffrées sont conservés. Le GC réécrit seulement les packs partiellement morts et retire les packs entièrement morts après validation des versions conservées.

Le GC parcourt les inventaires en flux et utilise un bit par objet empaqueté pour le marquage. Son résultat ajoute `packed_mark_bytes`, `loose_live_objects` et `peak_pending_objects` : octets utiles du bitmap initial, nombre de clés vivantes absentes des packs et maximum de clés en attente de parcours. Ces compteurs ne représentent pas toute la RAM du processus. Les clés vivantes isolées et les métadonnées restent en mémoire ; voir les [budgets du GC](PACKS.md#mémoire-du-gc).

Cette maintenance exige le verrou exclusif ; arrêter un serveur qui utilise le même store avant de la lancer. Elle active la configuration physique v2 pour un seul pack, puis v3 pour plusieurs packs ; les anciens binaires refusent les formats qu'ils ne comprennent pas. Les stores existants ne sont pas convertis à l'ouverture. Les index sont lus par pages, avec 1 048 576 objets maximum par pack, 1 024 packs et 4 194 304 objets empaquetés au total. Produire de nouveaux packs nécessite de l'espace avant de supprimer les anciens. Voir les [garanties de publication, reprise et limites](PACKS.md).

## Historique autonome

Copier les exclusions Unreal de `examples/unreal.syrnckignore` dans `.syrnckignore`, après les avoir adaptées au projet. Le store natif doit se trouver dans le dossier commité, normalement `.syrnck` à la racine.

```text
syrnck native commit --message "Import du projet"
syrnck native commit --message "Modification de la carte"
syrnck native log
syrnck native branch release <identifiant-revision>
syrnck native commit --branch release --message "Préparation de la release"
syrnck native export <identifiant-revision> ../export-release
```

`--branch` choisit la branche à mettre à jour ; il n'y a pas encore de branche courante, d'index de staging ou de commande de checkout natif. Chaque commit scanne tout le dossier. L'export exige un nouveau dossier dont le parent existe. Les fichiers doivent être stables pendant la lecture.

Pour raccourcir l'historique tout en gardant une archive :

```text
syrnck native detach-history --message "Nouveau départ"
syrnck refs
syrnck gc
```

L'ancienne tête est conservée sous `archive/<ancienne-tête>`. Pour abandonner réellement cette archive :

```text
syrnck unpin archive/<ancienne-tête>
syrnck gc
syrnck gc --delete
```

Les branches, autres archives et pins continuent de protéger leurs données. Après retrait de toutes les racines et nettoyage, les versions supprimées ne sont plus récupérables depuis ce store.

## Git et assets

Dans un dépôt Git existant :

```text
syrnck git install
```

Le store est créé dans le répertoire Git commun sous `syrnck`. Le chemin exact est affiché à l'installation ; `--store` permet ensuite de l'utiliser avec le CLI. La configuration est locale au dépôt et pointe vers le chemin absolu du binaire. Après déplacement du binaire, les chemins des filtres doivent être corrigés dans la configuration Git locale.

Ajouter par exemple dans `.gitattributes` :

```gitattributes
*.uasset filter=syrnck -diff -merge -text
*.umap filter=syrnck -diff -merge -text
```

Puis utiliser Git :

```text
git add .gitattributes Content/Map.uasset Source/Game.cpp
git commit -m "Carte et code"
git switch -c feature
```

Le contenu de `Source/Game.cpp` reste dans Git ; `Content/Map.uasset` est représenté dans Git par un pointeur et reconstruit dans le workspace. Pour convertir des fichiers déjà suivis, appliquer les attributs avec `git add --renormalize` aux chemins concernés et examiner le diff avant de commiter. Ne pas superposer filtres LFS et Syrnck sur les mêmes chemins.

Les pins `git/<manifeste>` protègent les assets même si un commit disparaît du graphe Git. Leur retrait est manuel dans ce prototype. Git conserve ses propres règles pour l'historique de code, indépendamment du GC Syrnck.

Le transfert des blocs est encore explicite, par référence de manifeste :

```text
syrnck --store <store-git> refs
syrnck --store <store-git> agent push https://blocs.exemple.test git/<manifeste> asset-release
```

Sur le destinataire, installer le filtre puis récupérer les blocs avant de demander à Git de matérialiser l'asset :

```text
syrnck git install
syrnck --store <store-git-destinataire> agent pull https://blocs.exemple.test asset-release git/<manifeste>
```

Le checkout ne télécharge pas encore les blocs manquants et un `git push` ne les envoie pas. Pour cloner l'historique avant d'avoir les blocs, utiliser `git clone --no-checkout`, installer Syrnck, récupérer les références d'assets nécessaires, puis effectuer le checkout. La publication groupée des assets d'un commit et les hooks sont à développer.

## Serveur et agent

Créer un store serveur séparé, puis définir un jeton d'au moins 32 caractères dans `SYRNCK_TOKEN`, identique sur les clients autorisés. Ne pas placer le jeton dans l'URL. Le CLI accepte `--token`, mais l'environnement évite sa présence dans la ligne de commande.

```text
syrnck --store serveur-data init
syrnck --store serveur-data serve --listen 127.0.0.1:7437
```

Dans un autre terminal, depuis le projet client :

```text
syrnck agent push http://127.0.0.1:7437 heads/main main
```

Depuis un autre store client initialisé :

```text
syrnck agent pull http://127.0.0.1:7437 main heads/received
syrnck native log --branch received
syrnck fsck
```

L'agent est une commande ponctuelle. Il ne surveille pas le disque et ne résout pas de conflits. Une interruption laisse des objets réutilisables mais aucune nouvelle référence incomplète. Relancer reprend à partir des objets présents. Une interruption peut laisser des objets non épinglés ; le serveur conserve son verrou pendant son exécution et n'effectue pas de GC en ligne.

`agent sessions` affiche les journaux locaux persistants. `agent resume URL SESSION_ID` reprend la racine et la référence attendue enregistrées, même si la référence source a disparu. Une session inachevée bloque le GC local ; `agent forget SESSION_ID` abandonne son journal et libère cette protection. Une publication distante incertaine est vérifiée sans renvoyer aveuglément le CAS. Voir les [garanties des sessions](TRANSFER_SESSIONS.md).

Les transferts acceptent `--requests 4 --transfer-buffer-mib 64`, `--retries 0..8` (3 par défaut), `--upload-kib-s` et `--download-kib-s` (Kio/s partagés, 0 pour illimité). Les attentes sont annulables ; la publication finale de référence n'est jamais relancée automatiquement. Voir la [politique de reprise et de débit](TRANSFER_POLICY.md).

Pour un accès distant, publier le serveur derrière un proxy HTTPS. Le client refuse HTTP hors loopback et les redirections. Un certificat d'autorité privé peut être fourni avec `--ca racine.pem`. Le prototype ne fournit pas encore de configuration de déploiement ou de gestion de certificats.

Les envois négocient automatiquement les baux persistants quand le serveur les annonce. `agent push ... --require-lease` exige leur prise en charge ; `serve --require-lease --lease-seconds 3600` impose les publications protégées. Après arrêt du serveur, `leases list`, `leases release ID` et `leases prune` permettent l'administration. Un bail actif bloque le GC serveur ; son expiration autorise la collecte des objets non référencés. Voir les [baux d'envoi, jetons et reçus](SERVER_UPLOAD_LEASES.md).

Les pulls négocient aussi un [bail de lecture](SERVER_READ_LEASES.md) avant leur première demande de bloc manquant. `agent pull ... --require-read-lease` ou `agent resume ... --require-read-lease` exige cette prise en charge. `serve --read-lease-seconds 3600` règle leur durée. Après arrêt du serveur, les commandes `leases list --read`, `leases release ID --read` et `leases prune --read` administrent ces baux. La racine reste protégée même sans référence tant que le bail est actif ; le client le libère lorsque tous les objets sont locaux, avant la publication de la référence locale.

## Chiffrement au repos

Exemple PowerShell :

```powershell
$env:SYRNCK_KEY = (& syrnck keygen)
syrnck --store donnees-chiffrees init --encrypted
syrnck --store donnees-chiffrees put Content/Map.uasset
```

Conserver cette clé dans un gestionnaire de secrets avant de fermer le terminal. Le même `SYRNCK_KEY` doit être fourni à chaque ouverture du store chiffré. Un store non chiffré doit s'ouvrir sans cette variable. Il n'existe pas encore de commande de conversion, rotation ou récupération des clés.

Chaque store peut employer sa propre clé. L'agent transmet le contenu déchiffré via le transport sécurisé ; le store destinataire le chiffre avec sa propre clé. Il ne s'agit pas de chiffrement de bout en bout contre le serveur.

## Interface C

Compiler les cibles `syrnck_ffi` et `syrnck_ffi_static` produit une DLL sous Windows, une bibliothèque dynamique sous Linux/macOS et une bibliothèque statique. Inclure `include/syrnck.h`, utiliser des chemins UTF-8 et fermer les handles. Les fonctions retournent un code d'état ; `syrnck_last_error()` donne le dernier message du thread. Le handle ne doit pas être utilisé simultanément par plusieurs threads.

Le manifeste JSON expose les identifiants et tailles des blocs. L'ABI C 1 fournit aussi leur présence, lecture/importation vérifiée, la fermeture des dépendances, les pins et la vérification du store. Le client réseau reste dans la bibliothèque C++. Voir le [binding Go](../bindings/go/README.md) pour une copie différentielle entre stores.
