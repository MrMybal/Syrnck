---
id: architecture
title: Architecture
description: Guide : Architecture.
section: Premiers pas
order: 10
sourceRevision: 7
---

# Architecture de Syrnck {#architecture-de-syrnck}

## Objectif et décisions {#objectif-et-decisions}

Un même moteur doit servir les assets lourds, le contrôle de versions autonome et la synchronisation de fichiers. Le code source doit aussi garder ses diff, merge, rebase, blame, tags, hooks et outils de revue. Le mode Git délègue ces fonctions à Git ; le mode autonome les développe progressivement au-dessus des mêmes objets.

C++20 est retenu pour une bibliothèque native portable, une intégration au développement Unreal et une interface C utilisable depuis Go. CMake construit les bibliothèques et exécutables ; RAII, les tests et les sanitizers encadrent la gestion de mémoire. La bibliothèque de stockage ne dépend ni de Git, ni d'HTTP, ni d'un watcher. Le CLI et les adaptateurs dépendent de la bibliothèque, et non l'inverse.

```diagram
{"type":"flow","title":"Syrnck","nodes":[{"id":"git","label":"Git / filters","x":0,"y":0},{"id":"cli","label":"CLI / native history","x":250,"y":0},{"id":"ffi","label":"C / Go / BEP adapter","x":500,"y":0},{"id":"core","label":"C++20 core","x":250,"y":140},{"id":"cas","label":"CAS / manifests / revisions","x":0,"y":280},{"id":"refs","label":"Refs / retention","x":250,"y":280},{"id":"net","label":"HTTP / block server","x":500,"y":280}],"edges":[{"from":"git","to":"core"},{"from":"cli","to":"core"},{"from":"ffi","to":"core"},{"from":"core","to":"cas"},{"from":"core","to":"refs"},{"from":"net","to":"core"}]}
```

Ce découpage rejoint un principe exposé dans la [conception de Lore](https://epicgames.github.io/lore/explanation/system-design/) : le stockage peut être consommé séparément du contrôle de versions. Il ne crée aucune compatibilité avec les formats Lore.

## Fichiers et révisions {#fichiers-et-revisions}

Un bloc est identifié par BLAKE3 avec un domaine séparant les types d'objets. Un manifeste contient la taille du fichier, son hash complet, la politique de découpage et la liste **ordonnée et complète** des blocs. Deux versions peuvent partager un bloc sans partager une relation de parenté.

Une révision autonome contient un arbre logique complet `chemin → manifeste + métadonnées`, ses parents et son message. Le prototype représente l'arbre dans une table triée ; une arborescence Merkle paginée remplacera cette représentation pour les grands dépôts. Chaque révision reste un état complet, même après ce changement de représentation.

Exemple :

```text
Version A : [bloc 1][bloc 2][bloc 3][bloc 4]
Version B : [bloc 1][bloc X][bloc 3][bloc 4]
```

La nouvelle version stocke et transmet le bloc X et son manifeste. Reconstituer B ne demande pas de rejouer A. Si A est supprimée, les blocs 1, 3 et 4 restent vivants tant que B les référence.

Le découpage ne comprend pas les objets Unreal. Une modification de quelques octets peut modifier un bloc entier, parfois plusieurs. Une réécriture, une compression ou un chiffrement préalable d'un asset peut modifier presque tous ses octets et réduire fortement la déduplication.

## Politique de découpage {#politique-de-decoupage}

Le prototype propose FastCDC 2020 avec minimum 256 Kio, moyenne cible 1 Mio et maximum 4 Mio, ainsi que des blocs fixes configurables. FastCDC permet de retrouver des frontières communes après une insertion ou une suppression ; la taille moyenne cible n'est pas une taille garantie. Le choix sera ensuite configurable par type de fichier et mesuré sur corpus.

[Lore documente son choix de FastCDC](https://epicgames.github.io/lore/developing/decisions/00001-fast-cdc/). À l'inverse, le [BEP de Syncthing](https://docs.syncthing.net/specs/bep-v1.html) utilise des blocs de 128 Kio à 16 Mio, de taille constante dans un fichier sauf le dernier. Le mode fixe de Syrnck ne suffit donc pas à rendre ses objets ou son réseau compatibles avec Syncthing : les hashes, métadonnées, messages, identités et règles de version sont aussi différents.

## Mode Git {#mode-git}

Les fichiers texte restent des blobs Git ordinaires. Les assets sélectionnés par `.gitattributes` passent par `clean`, qui écrit un pointeur vers un manifeste. `smudge` reconstruit les octets lors du checkout. Les filtres sont déclarés obligatoires : une erreur de stockage ne doit pas faire croire à Git qu'un asset a été correctement reconstruit.

L'adaptateur utilise un format de pointeur Syrnck, distinct de [Git LFS](https://raw.githubusercontent.com/git-lfs/git-lfs/main/docs/spec.md). GitHub et GitLab peuvent héberger l'historique Git contenant ces pointeurs, mais ne fournissent pas automatiquement le service de blocs Syrnck, les aperçus d'assets ou une interface LFS compatible.

La [migration LFS hors ligne](doc:git-lfs-migration) de `syrnck_git` lit un snapshot brut, vérifie les objets SHA-256 pendant leur ingestion et reconstruit uniquement les arbres Git modifiés. Une racine complète protège les assets avant la création d'une branche par transaction Git préparée. Le contrôle de la référence sous verrou protège aussi les références symboliques pendantes. HEAD, index et workspace restent indépendants de cette opération ; les anciens commits ne sont pas réécrits dans ce mode snapshot. La [migration complète](doc:git-lfs-history) convertit aussi l’ascendance dans de nouvelles références.

Chaque manifeste complet passé dans le filtre est épinglé. Un pointeur canonique non hydraté est conservé sans créer de pin incomplet, pour permettre les clones à pointeurs seuls. La [réconciliation de rétention](doc:git-retention) inspecte explicitement branches, tags, reflogs, stashes et index de chaque worktree, vérifie les fichiers complets puis ajoute les pins manquants en une transaction. Les pins non observés restent conservés ; leur retrait automatique coordonné reste à développer. Les [catalogues Git](doc:git-assets) associent une racine de révision sans parent à chaque commit, avec conservation locale et publication distante immuable. `syrnck_git_net` relie ces catalogues aux transferts réseau et vérifie les pointeurs avant publication locale. Un hook pre-push installé explicitement envoie les catalogues de toute l'ascendance avant la publication Git. Les politiques `local`/`download`/`skip` règlent le checkout : le téléchargement opt-in utilise le manifeste du pointeur, un bail de lecture et les sessions persistantes, sans dépendre de HEAD. Avant reconstruction, smudge vérifie taille, blocs et hash complet. Le [filtre résident](doc:git-filter-process) conserve un Store et son cache vérifié entre les fichiers d'une commande Git ; les requêtes restent séquentielles, sans hydratation différée. Le hook local n'assure pas une transaction distribuée avec le serveur Git.

## Mode autonome {#mode-autonome}

Le prototype offre la création de révisions, des branches nommées, un journal par premier parent et un export vers un nouveau dossier. Il permet aussi de détacher l'historique en gardant l'état courant et en archivant l'ancienne tête.

Les [comparaisons natives](doc:native-comparison) ajoutent un `status` du dossier par hash complet et un `diff` des métadonnées de snapshots, sans import ni publication. Le [staging natif](doc:native-staging) conserve une sélection immuable sous `staging/BRANCHE`, protégée contre le GC avec sa base. Le commit préparé vérifie ses dépendances puis déplace la branche et consomme le staging en une transaction de références ; une base périmée est refusée. Les [patches](doc:text-patches), [merges à trois voies](doc:text-merge), [merge natif](doc:native-merge), [rebase linéaire/interactif](doc:native-rebase), [cherry-pick/revert](doc:native-replay), [stash](doc:native-stash), [blame et renommages exacts](doc:native-file-history), [bisect](doc:native-bisect) sont disponibles avec leurs limites documentées. Les [révisions avec auteurs/signatures](doc:native-authorship) et l’[alignement des renommages modifiés](doc:native-file-history#edited-renames) sont disponibles sur activation explicite. Le rebase préservant les merges reste à réaliser.

L'[inspection du workspace](doc:native-workspace) combine base/index/dossier et prépare un plan de checkout sans écriture. Ce plan distingue contenu, mode et représentation, impose les fichiers suivis propres, et inventorie aussi les chemins ignorés pour détecter les collisions. Liens/jonctions, collisions ambiguës et répertoires protégés bloquent ; les [transitions propres de casse et fichier/dossier](doc:native-checkout#recoverable-namespace-transitions) sont journalisées. La vérification complète des objets cibles reste optionnelle pour le plan.

Le [checkout récupérable](doc:native-checkout) vérifie les fermetures source/cible, épingle un journal lié au chemin canonique et prépare des fichiers complets avant les déplacements exclusifs. Les originaux restent dans des slots privés jusqu'à publication atomique de la branche, de l'archive source et de la rétention cible. Une interruption se reprend ou s'annule explicitement ; une publication déjà engagée ne peut que terminer son nettoyage. Les déplacements du workspace sont séquentiels, sans verrou contre les éditeurs externes. Les fichiers privés restent en clair même dans un store chiffré. Le GC et les écritures natives du workspace sont bloqués tant que le journal est actif ; les archives/reçus restent conservés sans purge automatique.

Le [HEAD natif](doc:native-branches) est un chunk canonique sous `workspace/HEAD`, lié au workspace et à une sélection ; son absence sélectionne implicitement main. Les descripteurs nommés gardent v1 ; [HEAD détaché](doc:native-detached) utilise v2 et des racines privées de révision/index. Le changement de branche réutilise le journal v2 (v4 pour les transitions de namespace), ou v7 avec cible détachée, avec les HEAD attendu et nouveau. La publication finale sélectionne HEAD avec le journal terminé en vérifiant et conservant les deux têtes. Les commandes CLI par défaut et `Store::native_target()` résolvent la sélection. Les anciens reçus restent lisibles. Les [workspaces liés](doc:native-workspaces) placent HEAD/index/cache/journal et reçus sous les vraies racines `workspaces/NOM/`, partagent objets/branches et réservent les branches sélectionnées. Leurs reçus v8 enregistrent contexte et layout ; les sauvegardes gardent des sélections portables plutôt que les chemins absolus. Les workspaces partiels statiques conservent révisions/index logiques complets et projettent les fichiers physiques selon une sélection canonique. Leurs reçus v9 figent les deux sélections ; changement dynamique et administration des workspaces restent à compléter.

Les fichiers source utilisent actuellement le même moteur de stockage en mode autonome. Leurs interfaces de [diff](doc:text-diff) et [merge](doc:text-merge) textuels sont explicites et bornées. L'utilisation de blocs pour les stocker n'oblige pas à présenter des différences de blocs à l'utilisateur.

Les [tags natifs](doc:native-tags) utilisent une référence directe vers la révision et une référence optionnelle vers un chunk d'annotation. La publication des deux est atomique. La suppression archive la cible, l'annotation et un reçu canonique avec une vérification de l'ID attendu ; leur restauration rétablit le nom et le message sans retirer l'archive. Ces racines continuent de protéger les versions pendant le GC. Les sélecteurs CLI résolvent HEAD, branche ou tag et refusent les noms ambigus. La liste paginée borne son JSON sous 16 Mio, en plus du nombre d'entrées demandé. Les signatures, politiques serveur et transferts atomiques de groupes de racines restent à développer.

## Stockage physique et packs {#stockage-physique-et-packs}

Les objets logiques v1 peuvent être stockés dans des fichiers isolés ou dans des packs immuables indexés. `Store::pack()` et `syrnck pack` ajoutent des packs pour les seuls nouveaux objets ; `pack --compact` regroupe tout le stock. La configuration physique passe à v2 pour un seul pack, puis v3 pour un catalogue de plusieurs packs ; les nouveaux stores restent v1. Le remplacement atomique de `config.json` publie le catalogue complet après écriture et relecture vérifiée, avant tout retrait. Le GC préserve les packs entièrement vivants, retire ceux entièrement morts et réécrit les seuls packs partiellement morts. Les références et les blocs partagés conservent les mêmes règles de rétention.

Les index sont lus par pages de 256 entrées. Un répertoire de bornes/empreintes et un filtre de clés absentes remplacent la table complète ; une LRU de pages de 4 Mio est configurable indépendamment du cache décodé. L'ouverture vérifie encore chaque index complet et les doublons entre packs par parcours fusionnés. Les limites restent 1 048 576 entrées par pack, 1 024 packs et 4 194 304 entrées au total. Le GC et `fsck` parcourent les inventaires en flux, avec un bit de marquage par objet empaqueté ; le GC recopie également les survivants en flux. Les clés parcourues absentes des packs, la pile de parcours et le JSON restent en mémoire ; `objects()` et l'API publique `closure()` gardent encore leurs collections d'identifiants. L'empaquetage et la compaction fusionnent les index par pages et gardent seulement les clés isolées à trier ; voir les [garanties de compaction](doc:compaction). Une session réutilise au plus 8 handles de packs avec LRU et les ferme avant publication/suppression. La maintenance conserve le verrou exclusif. L'index ne dispense pas de vérifier les enveloppes réellement lues, et `fsck` contrôle aussi les doublons physiques. Voir les [garanties des audits](doc:audit), les [budgets des index](doc:indexes) et les [étapes de reprise](doc:packs).

## Pipeline d'ingestion {#pipeline-d-ingestion}

L'ingestion C++ peut utiliser un pool de 2 à 64 workers pour la compression et le chiffrement des blocs nouveaux. Le coordinateur conserve le découpage CDC, les hashes, la détection des doublons, l'ordre du manifeste et les écritures atomiques. Les workers ne modifient ni le store ni les références ; chacun possède son contexte Zstd réutilisable. Un budget d'octets et un plafond de `2 × workers` tâches imposent l'attente du producteur quand la file est pleine. Le mode séquentiel reste le choix par défaut.

Les buffers bruts sont libérés avant que le résultat d'encodage soit livré au coordinateur. À ce budget s'ajoutent les buffers compressés/chiffrés, le tampon de lecture, les contextes Zstd par worker et les métadonnées. Le manifeste plat est borné pendant la lecture par sa limite sérialisée de 16 Mio ; cette limite ne représente pas sa consommation en mémoire sous forme JSON.

La publication des blocs suit leur première apparition dans le fichier. Le manifeste n'est créé qu'après la réussite de toutes les tâches. Les exceptions des workers remontent au coordinateur ; lecture en échec, écriture refusée et annulation coopérative arrêtent les tâches restantes et joignent les threads. Les références existantes restent intactes. Des blocs orphelins déjà publiés peuvent subsister jusqu'à leur réutilisation ou leur collecte. Le verrou exclusif externe du store reste nécessaire ; le parallélisme interne ne rend pas le handle utilisable simultanément par plusieurs appelants.

## Cache et parcours de validation {#cache-et-parcours-de-validation}

Le cache est local au handle et ne constitue pas une source d'autorité sur disque. Une entrée n'est créée qu'après validation complète d'un objet lu : déchiffrement/authentification si nécessaire, décompression bornée, ID et format. Elle associe la clé typée à l'empreinte BLAKE3 de l'enveloppe exacte. Les accès suivants relisent et hachent cette enveloppe avant de réutiliser l'entrée ; un simple mtime ou une taille ne suffit pas. Une empreinte différente évince l'entrée et impose une nouvelle validation complète. Une absence sur disque reste une absence, même si l'objet était en cache.

Deux LRU indépendants bornent les empreintes et les données décodées. Le défaut est 8 192 entrées et 64 Mio de données, hors structures d'index et buffers de travail. L'absence de place pour une donnée n'empêche pas de conserver son empreinte. Le cache ne conserve pas les objets contre le GC et ne persiste pas entre ouvertures ; ses buffers décodés sont nettoyés avant libération. Le chiffrement au repos reste distinct de la présence de données déchiffrées dans un processus autorisé.

La validation complète d'une fermeture parcourt chaque manifeste et vérifie son hash de fichier en lisant ses blocs dans le même passage. Les révisions, manifestes et blocs rencontrés alimentent l'ensemble des dépendances ; les parents et versions protégées restent couverts. Les blocs partagés entre plusieurs fichiers peuvent être relus pour vérifier chacun de leurs hashes complets. `fsck` réunit les objets énumérés et les racines dans une seule traversée, et `gc` valide toutes les racines avant toute suppression. Ces deux opérations contournent le cache, avec restauration de son mode normal même après une erreur.

## Conservation et suppression {#conservation-et-suppression}

Les racines de rétention sont les branches, archives, pins et références reçues. Le GC marque leurs dépendances transitives puis supprime les objets inaccessibles. Il conserve aussi les parents des révisions vivantes. Un verrou exclusif du store empêche qu'un autre processus publie une référence ou ajoute des objets pendant le nettoyage. Un objet vivant manquant ou corrompu interrompt le GC avant toute suppression.

Trois opérations doivent rester distinctes :

1. Retirer un commit d'une branche : son contenu peut rester dans une archive, un autre historique ou un pin.
2. Retirer une référence de rétention : ses objets deviennent éligibles au GC s'ils ne sont plus partagés.
3. Exécuter le GC : les objets réellement inaccessibles sont supprimés physiquement.

`native detach-history` archive l'ancienne tête avant de déplacer la branche. Pour libérer ensuite les données anciennes, il faut retirer cette archive puis lancer le GC. Une version volontairement supprimée de toutes les racines et nettoyée ne peut plus être reconstruite. Une suppression définitive et une conservation éternelle des mêmes données seraient incompatibles.

Les [journaux de transfert](doc:transfer-sessions) protègent le store local et les [baux d'envoi serveur](doc:server-upload-leases) protègent les uploads jusqu'à expiration ou publication. Pour un déploiement réel, compléter les délais de grâce, politiques de rétention, pins de releases et backups vérifiés. Le GC distribué ne pourra pas se contenter des racines d'un seul client.

## Synchronisation et serveur {#synchronisation-et-serveur}

L'agent compare des identifiants d'objets, demande les absents puis transfère leur contenu. Les objets reçus sont vérifiés, stockés et réutilisables lors d'une reprise. La nouvelle référence devient visible seulement lorsque toute sa fermeture de dépendances a été validée. Une comparaison avec la valeur attendue protège la publication contre un écrasement concurrent.

Le serveur est mono-store avec jeton partagé historique ou [utilisateurs/permissions](doc:server-access) ; il sérialise les accès au stockage. Le GC et les autres processus locaux sont exclus pendant son exécution. Le réseau limite les corps à 16 Mio, les requêtes de présence à 4 096 objets et le pool serveur à 16 threads. L'agent peut chevaucher des requêtes d'objets avec des [plafonds de concurrence et de buffers](doc:parallel-transfers), tout en coordonnant les accès au Store.

Un transfert de blocs ne règle pas les conflits entre deux modifications de fichiers. L’[agent natif résident](doc:resident-agent) échange les révisions protégées et peut fusionner les historiques compatibles. Le [contrôleur BEP](doc:syncthing) retient vecteurs/tombstones et peut préserver les fichiers ordinaires concurrents. Publication multi-serveur, métadonnées de dossiers/liens et rétention distribuée restent à terminer.

Les [baux de lecture persistants](doc:server-read-leases) protègent la racine exacte d'un pull contre le GC hors ligne tant qu'ils sont actifs, même après suppression de la référence source. Le client les renouvelle par une requête indépendante, puis les libère lorsque tous les objets sont locaux. Le GC protège sélectivement les fermetures complètes des lectures et sources locales, ainsi que les objets disponibles des pulls incomplets. Les [inventaires d'envoi](doc:upload-inventories) ajoutent les objets déclarés avant les demandes de présence, même sans manifeste ; leurs pages chaînées sont validées avant toute suppression. Les anciens baux d'envoi gardent une protection globale. Le GC distribué reste à développer. Voir la [rétention et la purge des reçus](doc:selective-retention).

L'interface C expose l'ingestion, la reconstruction, la présence/lecture/importation des objets, leur fermeture de dépendances et les pins vérifiés. Le binding `bindings/go` utilise cette interface par DLL sous Windows et cgo sous Unix. Son banc de copie entre stores valide le transfert différentiel et la reprise sous Windows x64. L’[adaptateur BEP](doc:syncthing) utilise la bibliothèque Syncthing et des blocs fixes SHA-256 en conservant le stockage propre du moteur. Il ne fournit pas un daemon complet ni de protocole CDC entre pairs Syncthing.

## Chiffrement {#chiffrement}

Le prototype compresse puis chiffre chaque objet au repos avec XChaCha20-Poly1305, une clé aléatoire de 256 bits et un nonce aléatoire de 192 bits. L'identifiant et le type de l'objet sont authentifiés comme données associées. Les manifestes et révisions sont aussi chiffrés ; la configuration et les noms de références restent visibles. Les identifiants de contenu et tailles stockées ne sont pas masqués.

Le CLI choisit `SYRNCK_KEY` ou un [fichier de clé protégé par mot de passe](doc:key-files#providers), pas une clé dans la configuration du Store. Les hôtes C++ peuvent utiliser séparément l'[API de coffre système](doc:key-files#system-vault) ; sa sélection comme fournisseur CLI/C/Go reste à terminer. Rotation en place/distribuée et récupération demandent encore du travail. Le détenteur des droits sur le processus et son environnement reste un acteur de confiance.

La [compression réseau négociée](doc:network-compression) utilise Zstandard niveau 1 sur les objets logiques, avec repli brut si le gain ou le budget ne suffit pas. Le décodage vérifie la trame avant import et écrit directement dans le buffer réservé. Les compteurs et cibles de débit réseau incluent les tentatives répétées ; les octets logiques réussis gardent leur compteur distinct.

Les transferts ordinaires déchiffrent les objets pour un serveur de confiance via HTTPS lorsqu'il est distant. Le [workflow chiffré explicite](doc:sealed-repositories) chiffre au contraire objets/métadonnées côté client et transfère des supports opaques, avec adresses d'égalité à clé et points locaux contre le retour en arrière. Confiance initiale, intégration résidente/BEP et révocation distribuée des appareils/clés restent à terminer.

Le [mode appareils non fiables de Syncthing](https://docs.syncthing.net/specs/untrusted.html) illustre un autre compromis : son chiffrement de blocs dépend du fichier et de l'offset, ce qui limite les corrélations. Reprendre ce choix tel quel empêcherait la déduplication globale entre fichiers et positions visée ici. Son transport BEP utilise TLS et l'identité des certificats ; le prototype Syrnck utilise un jeton et un proxy TLS, sans identité d'appareil Syncthing.

## Intégrité et limites actuelles {#integrite-et-limites-actuelles}

Les objets utilisent un fichier temporaire puis une publication atomique. Les références sont remplacées atomiquement après validation des dépendances. La reconstruction vérifie chaque bloc et le hash complet du fichier ; l'export d'un fichier ne remplace pas une destination existante. L'export d'une révision entière prépare une arborescence complète puis la publie par renommage exclusif dans un nouveau dossier. Un arrêt forcé peut laisser son dossier temporaire. Le checkout et le changement de branche dans un workspace existant utilisent un journal de reprise et des déplacements par fichier, sans atomicité globale du dossier.

Le scanner exclut `.git`, `.syrnck`, le store et les règles `.syrnckignore`. Il refuse actuellement symlinks et fichiers spéciaux. Les chemins doivent être UTF-8 et portables ; utf8proc détecte leurs collisions après normalisation et casefold Unicode. Le prototype n'enregistre pas les ACL, xattrs, liens matériels ou répertoires vides. Les droits exécutables sont conservés sur Unix seulement.

Le scanner vérifie taille et date de modification autour de chaque lecture. Il ne fournit pas un snapshot atomique du système de fichiers : les fichiers doivent être stables pendant un commit. Les garanties après perte brutale d'alimentation, sur partages réseau ou face à un autre processus modifiant les fichiers du store en contournant son verrou restent à éprouver.

Le code original est sous MIT. Les [notices des dépendances](doc:licenses) conservent leurs licences propres.
