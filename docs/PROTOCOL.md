# Formats et protocole expérimentaux

Le format est un contrat initial à versionner, pas une promesse de compatibilité définitive. La version du format d'objet est distincte de la version du binaire et de l'ABI C.

## Identifiants et objets

L'identifiant est le BLAKE3 sur :

```text
ASCII("syrnck-object-v1") || 0x00 || ASCII(type) || 0x00 || contenu_non_compressé
```

Les types autorisés sont `chunk`, `manifest`, `revision`. L'ID contient exactement 64 caractères hexadécimaux minuscules. La clé complète est `{kind, id}`. L'espace de noms sépare donc un bloc des métadonnées contenant les mêmes octets.

Un bloc contient de 1 octet à 16 Mio. Un fichier vide possède zéro bloc et le hash BLAKE3 du flux vide. Tous les objets non compressés sont limités à 16 Mio, y compris les métadonnées.

Les manifestes sont du JSON UTF-8 compact, sans espaces superflus ni newline finale. Les champs sont écrits dans cet ordre : `version`, `size`, `file_hash`, `chunking`, `blocks`. Chaque bloc contient `id`, puis `size`. La configuration CDC contient `algorithm: "fast-cdc2020"`, `min`, `avg`, `max` ; le mode fixe contient `algorithm: "fixed"`, `size`. Le nom sérialisé exact fait foi et est vérifiable avec `inspect manifest`.

La politique CDC C++ reproduit les frontières de `fastcdc::v2020::StreamCDC::new` de fastcdc 5.0.0, normalisation par défaut niveau 1, seed par défaut 0. Les paramètres sont enregistrés pour ne pas dépendre du choix par défaut d'une future version du CLI. Une modification des frontières produites exigera une nouvelle valeur d'algorithme ou de format.

Le hash `file_hash` est le BLAKE3 ordinaire des octets complets du fichier, sans domaine. La somme des tailles de blocs doit égaler `size`. La reconstruction vérifie aussi les tailles réelles et ce hash.

Le nombre de workers et le budget de l'ingestion ne sont pas sérialisés dans le manifeste : ils ne modifient ni les frontières, ni l'ordre des blocs, ni leurs identifiants. La taille sérialisée du manifeste plat est contrôlée pendant la lecture, avant sa publication. Les statistiques d'ingestion exposent séparément `peak_in_flight_bytes` et `peak_in_flight_blocks` ; ces compteurs ne font pas partie du format d'objet.

Les révisions sont du JSON compact dans l'ordre `version`, `parents`, `message`, `timestamp`, `files`. Les chemins de `files` sont triés selon l'ordre des chaînes UTF-8. Une entrée contient `manifest`, puis `executable`. L'échappement des chaînes conserve le format v1 du prototype initial, vérifié par les fixtures de migration et produit par `nlohmann::ordered_json` ; tout document dont la re-sérialisation diffère est refusé. Les champs inconnus sont refusés. Une implémentation indépendante devra tester ses encodages contre des vecteurs de référence avant de produire des objets.

## Stockage local

```text
<store>/config.json
<store>/refs.json
<store>/store.lock
<store>/objects/<type>/<deux-premiers-caractères>/<id>
```

L'enveloppe binaire commence par `SYR1`, puis un octet de mode. Mode 0 : contenu Zstd. Mode 1 : nonce de 24 octets, puis chiffrement XChaCha20-Poly1305 du contenu Zstd, tag inclus. Les données associées sont l'ASCII de `syrnck-envelope-v1:<type>:<id>`. Un objet chiffré ne peut pas être déplacé sous une autre clé sans échec d'authentification.

Sans packs, `config.json` contient `version: 1` et `key_check`. Pour un store chiffré, `key_check` est BLAKE3 keyed du texte `syrnck-store-key-check-v1` avec la clé du store ; sinon il vaut `null`. Les noms de références et IDs restent en clair dans `refs.json`.

Le cache de lecture est uniquement en mémoire et n'ajoute aucun fichier ni champ à ce format. Ses entrées ne sont ni des racines de rétention ni une autorité permettant d'ignorer un objet manquant. Elles proviennent d'un objet validé dans le handle courant et ne sont réutilisées qu'après comparaison cryptographique de l'enveloppe relue. Le nombre de lectures et de décodages peut changer sans modifier les octets logiques des objets ou les règles de validation des hashes de fichiers.

### Stockage physique v2 avec packs

Pour un seul pack, l'appel explicite à `pack` peut publier une configuration de trois champs : `version: 2`, `key_check` inchangé, et `pack`. `pack` vaut `null` s'il n'y a aucun pack actif ; sinon il contient exactement `name` (32 caractères hexadécimaux minuscules), `objects` (nombre entier positif), `bytes` (taille du pack), `index_hash` (BLAKE3 ordinaire de tout l'index, hexadécimal minuscule). La configuration est le pointeur de publication atomique. Les objets logiques, références, enveloppes et messages réseau gardent leur format v1. Les anciens lecteurs de configuration v1 doivent refuser v2.

Les fichiers actifs se trouvent à `packs/pack-<name>.syp` et `packs/pack-<name>.syi`. Le pack commence par les 8 octets ASCII `SYRPACK1`, suivis des enveloppes `SYR1` concaténées sans modification. Il n'y a ni espace libre ni donnée non indexée dans une génération. Tous les entiers binaires sont non signés en little-endian.

L'index commence par les 8 octets ASCII `SYRIDX01`, puis un entier de 8 octets donnant le nombre d'objets. Chaque entrée de 45 octets contient :

| Champ | Taille | Représentation |
| --- | ---: | --- |
| Type | 1 octet | 0 = chunk, 1 = manifest, 2 = revision |
| ID | 32 octets | Hash binaire, sans encodage hexadécimal |
| Offset | 8 octets | Position absolue du début de l'enveloppe dans le pack |
| Longueur | 4 octets | Taille de l'enveloppe complète |

Les entrées sont strictement triées par type puis ID ; aucun doublon n'est admis. Les plages doivent être contiguës depuis l'offset 8 jusqu'à `bytes`. La longueur d'une enveloppe est comprise entre 5 octets et 17 Mio, limite existante du lecteur d'objets. Le nombre d'entrées actuel est limité à 1 048 576 et la taille du pack à `INT64_MAX`. La taille de l'index doit être exactement `16 + 45 × objects` et son hash doit correspondre à la configuration.

L'ouverture et les audits rechargent et vérifient cet index. À l'ouverture d'une session de lecture du pack, le moteur contrôle le fichier, sa taille et son en-tête. Une reconstruction ou un audit réutilise ce handle pendant son opération puis le ferme ; les lectures isolées ouvrent leur propre session. Chaque objet relit les octets de l'enveloppe ciblée, dont le contenu doit ensuite satisfaire les mêmes règles d'authentification, décompression, ID et format que les fichiers isolés. Le cache ne dispense pas de relire l'enveloppe. Le hash d'index détecte les altérations mais ne constitue pas une signature authentifiée du catalogue. Les IDs, types, positions et tailles restent visibles dans l'index, même si les enveloppes sont chiffrées.

Les écritures nouvelles restent à `objects/...`. La lecture ordinaire privilégie un fichier isolé lorsqu'un objet existe aussi dans le pack ; l'inventaire retourne une seule clé logique. `fsck` vérifie les deux copies. Les fichiers de packs non désignés par la configuration n'ajoutent aucune racine de rétention. Voir [publication, reprise et compaction](PACKS.md).

### Stockage physique v3 : catalogue de packs

La configuration v3 contient exactement `version: 3`, `key_check` inchangé et `packs`, un tableau de descripteurs de packs au format v2 décrit ci-dessus. Le tableau peut être vide ou contenir un seul pack ; le moteur ne rétrograde pas implicitement un store v3. Plusieurs packs exigent v3, que les lecteurs v1/v2 doivent refuser. Les formats binaires `SYRPACK1` et `SYRIDX01` restent inchangés.

Les noms de packs doivent être distincts. Une clé `{kind, id}` ne peut figurer que dans un seul pack actif, même si deux copies physiques auraient un contenu identique. Les index sont vérifiés séparément, puis des parcours fusionnés contrôlent les clés lorsque leurs intervalles se recouvrent. Le catalogue est limité à 1 024 packs et à 4 194 304 objets au total, avec toujours au maximum 1 048 576 objets dans chaque index. Les tailles annoncées sont contrôlées avant la lecture des index ; les plages et contenus restent vérifiés ensuite.

La maintenance ordinaire ajoute les nouveaux packs au tableau. Une compaction explicite peut en remplacer tout le contenu ; le GC peut en remplacer seulement une partie. Tous les fichiers d'un lot sont écrits et vérifiés avant le remplacement atomique unique de la configuration. Un lot non publié reste inactif et ne remplace aucune ancienne copie. La présence d'un fichier hors catalogue n'ajoute aucun objet à l'inventaire.

Les sessions de reconstruction et d'audit réutilisent au plus 8 handles de packs, avec éviction LRU et fermeture en fin d'opération. Leur limite de handles est distincte des plafonds d'index et du cache décodé. La validation de chaque enveloppe est identique au stockage v1/v2.

### Consultation paginée sans changement de format

Le lecteur conserve le format plat `SYRIDX01` et le même hash global. À l'ouverture, il le parcourt en flux, construit des bornes et empreintes de pages de 256 enregistrements ainsi qu'un filtre de clés absentes. Ces structures et le cache de pages sont uniquement en mémoire ; aucun en-tête, descripteur ou fichier auxiliaire n'est ajouté au store. La construction des nouveaux index écrit également les enregistrements en flux, avec le même encodage binaire.

Les recherches consultent les pages candidates ; chaque page relue doit correspondre à l'empreinte du parcours validé avant utilisation de ses adresses. Les pages chaudes et la dernière adresse trouvée peuvent rester un snapshot du catalogue ouvert sous verrou, comme l'ancienne table en RAM. Les audits rechargent les index et recontrôlent leurs hashes complets, indépendamment de ce cache. La vérification des enveloppes réellement lues reste obligatoire avant de retourner un objet. Voir les [budgets, garanties et limites](INDEXES.md).

## Pointeur Git

```text
version syrnck-pointer-v1
manifest blake3:<64 caractères hexadécimaux>
size <entier décimal>
```

Les trois lignes se terminent par LF. L'encodage est canonique et le pointeur est limité à 1 024 octets. La taille doit correspondre au manifeste. Un document commençant par l'en-tête réservé mais mal formé est une erreur. Ce format est distinct des pointeurs Git LFS.

## API HTTP

Chaque requête porte `Authorization: Bearer <jeton>`. Le serveur initial expose un seul store et une seule autorité partagée. HTTPS est requis côté client, à l'exception de loopback. Le service HTTP écoute uniquement sur loopback et peut être exposé derrière un proxy TLS.

| Méthode et chemin | Corps / réponse |
| --- | --- |
| `POST /v1/missing` | Tableau JSON de clés ; retourne le sous-ensemble absent ; maximum 4 096 entrées |
| `GET /v1/objects/{kind}/{id}` | Octets logiques ou trame Zstandard négociée, maximum 16 Mio |
| `PUT /v1/objects/{kind}/{id}` | Même représentation ; vérifie type, hash et format ; 201 si créé, 204 si déjà présent |
| `GET /v1/refs` | Table JSON `nom → clé` |
| `PUT /v1/refs/{name}` | `{root: clé, expected: clé ou null}` ; valide toutes les dépendances avant publication |

Les noms distants utilisent un seul composant d'URL. Les références locales peuvent employer `/`. `expected: null` exige l'absence de référence ; une valeur exige une égalité exacte. Un conflit renvoie 409. Une dépendance absente renvoie 404 ; une corruption ou un objet invalide renvoie une erreur. Les réponses JSON peuvent employer n'importe quel ordre de champs : la contrainte d'encodage canonique porte sur les objets immuables, pas sur les enveloppes de requêtes.

Les objets peuvent arriver avant leurs dépendances. Ils ne sont pas publiés comme nouvelle version tant que la référence n'a pas été validée et remplacée. Un retry du PUT d'un objet déjà présent est idempotent. Le client désactive les redirections pour ne pas déplacer les requêtes authentifiées.

Cette v1 n'est compatible ni avec BEP, ni avec l'API Git LFS, ni avec le protocole Lore. L'extension négociée `GET /v1/capabilities` annonce les [baux d'envoi persistants](SERVER_UPLOAD_LEASES.md), leur durée et les reçus de publication. Les publications protégées ajoutent `upload: {id, fence}` au corps existant ; les requêtes historiques restent acceptées sauf si le serveur impose les baux. L'extension [baux de lecture](SERVER_READ_LEASES.md) annonce `read_leases: 1` et `read_lease_seconds`, avec acquisition/renouvellement sur `PUT /v1/reads/ID`, consultation sur `GET /v1/reads/ID` et libération sur `POST /v1/reads/ID/release`. Elle protège une racine exacte avant le téléchargement des blocs manquants. Les quotas d'objets/utilisateurs et la réplication serveur restent à développer.

L'extension [inventaires d'envoi](UPLOAD_INVENTORIES.md) annonce `upload_inventories: 1`. La création du bail ajoute `inventory: true` ; `PUT /v1/uploads/ID/inventory/INDEX` enregistre un lot de clés et son jeton avant la demande de présence. Les pages sont chaînées et leur nombre/empreinte finale sont conservés dans le bail de schéma 2. Les baux historiques de schéma 1 gardent leur protection globale. Le format logique des objets et l'ABI C restent inchangés.

L'extension de [compression réseau](NETWORK_COMPRESSION.md) annonce `object_zstd: 1`. Les en-têtes `Syrnck-Accept-Object-Encoding: zstd` et `Syrnck-Object-Encoding: zstd` négocient une trame par objet. Les clients historiques continuent de recevoir des octets logiques. Le client vérifie taille, fenêtre, intégrité et format avant import ; les corps JSON et la publication CAS gardent leur représentation existante.

## Interface C

L'en-tête `include/syrnck.h` définit l'ABI 1, annoncée par `syrnck_abi_version()`. Cette version est indépendante du format d'objet v1 et de la version 0.1.0 du produit. Les fonctions renvoient 0 en cas de succès, 1 pour une erreur et 2 pour une exception C++ inconnue. Aucune exception ne franchit l'interface. Les chaînes sont UTF-8 terminées par NUL ; les handles doivent être sérialisés, fermeture comprise. Aucun pointeur d'entrée n'est conservé.

`syrnck_object_has` vérifie un objet présent ; une corruption n'est pas assimilée à une absence. `syrnck_object_read` retourne les octets non compressés vérifiés. Une requête avec pointeur nul et capacité zéro donne la taille ; la lecture exige ensuite un buffer assez grand et n'écrit rien si sa capacité est insuffisante. `syrnck_closure_json` suit le même contrat, mais sa longueur inclut le NUL final. `syrnck_object_import` valide hash et format, sans exiger que les dépendances soient déjà présentes.

Après import de tous les objets, `syrnck_ref_set` valide les dépendances et les hashes complets avant de publier le pin. Cette fonction remplace une référence locale sans compare-and-swap ; l'API HTTP conserve son contrat CAS distinct. Les objets reçus avant publication ne sont pas automatiquement protégés du GC après fermeture du store. L'intégrateur doit garder le store ouvert et sérialiser transfert/GC, puis conserver une racine avant fermeture pour garantir la rétention.

`syrnck_last_error` est local au thread et reste valide jusqu'au prochain appel qui modifie l'erreur. Le diagnostic est borné à 4 095 octets pour éviter d'allouer de la mémoire pendant le signalement d'une erreur. `syrnck_error_copy` permet de le copier sans le modifier ; sa longueur inclut NUL. Un binding avec goroutines doit récupérer l'erreur sur le même thread OS que l'appel. Les buffers d'objets appartiennent à l'appelant ; seules les chaînes allouées par `syrnck_manifest_json` passent par `syrnck_string_free`.
