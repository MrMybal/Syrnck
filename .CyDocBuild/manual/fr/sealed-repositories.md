---
id: sealed-repositories
title: Dépôts chiffrés de bout en bout
description: Clés clientes séparées, snapshots opaques au serveur, transferts différentiels et contrôle des retours en arrière.
section: Storage
parent: storage
order: 57
sourceRevision: 1
---

## Ce que le serveur voit {#threat-model}

Le workflow explicite `sealed` chiffre blocs, manifestes et révisions originaux avant de les confier au serveur de transfert Syrnck ordinaire. Le client garde un Store en clair et un **Store de transport** distinct contenant le chiffré. Le serveur ne reçoit jamais la clé de bout en bout. Sa propre clé facultative au repos est indépendante et ne permet pas de déchiffrer les objets originaux.

Contenus, chemins originaux, messages de commit, noms des racines et IDs d'objets originaux sont chiffrés. La représentation de transport expose un canal public, des IDs opaques aléatoires/à clé, tailles/nombres d'objets, égalité des objets réutilisés, horaires de mise à jour et historique des supports. Les supports signés exposent aussi leurs auteurs/committeurs. Des objets typés originaux identiques partagent une adresse à clé dans un même domaine de clé maîtresse, y compris entre ses canaux. Utiliser des clés indépendantes pour des domaines de confidentialité indépendants. Il ne s'agit ni de résistance à l'analyse du trafic ni d'un système cryptographique audité extérieurement.

Les transferts ordinaires `agent`, natifs et Git gardent leur protocole d'objets décodés. Choisir une clé au repos n'active pas ce workflow. L'agent résident et le service BEP Syncthing ne consomment pas encore ces snapshots chiffrés.

## Créer et ouvrir une clé séparée {#key-files}

Fournir `SYRNCK_SEALED_KEY_PASSWORD` via un lanceur de confiance, puis créer un fichier protégé dans un répertoire privé existant hors du projet :

```text
syrnck sealed-key create private-keys/team.syreakey
syrnck sealed-key inspect private-keys/team.syreakey
syrnck sealed-key verify private-keys/team.syreakey
```

Définir `SYRNCK_SEALED_KEY_FILE` avec son chemin absolu et garder `SYRNCK_SEALED_KEY_PASSWORD` disponible au client. Sinon, définir `SYRNCK_SEALED_KEY` avec une clé existante de 32 octets encodée en 64 caractères hexadécimaux. Deux fournisseurs simultanés, un fournisseur vide ou des identifiants invalides/incorrects sont refusés. Le CLI vide ces variables de son propre environnement après chargement et avant qu'une commande sans rapport puisse lancer des enfants. L'environnement du lanceur reste inchangé.

`sealed-key protect FICHIER` protège la clé brute exacte. `sealed-key rewrap SOURCE CIBLE` utilise l'ancien mot de passe et `SYRNCK_NEW_SEALED_KEY_PASSWORD`, vers une nouvelle destination. Clé maîtresse et domaine public restent identiques. Les cinq actions acceptent les contrôles de variables de mot de passe/KDF applicables aux [clés du stockage](doc:key-files). L'enveloppe authentifiée de 104 octets possède une finalité distincte : les enveloppes de clés au repos ou de graines de signature sont refusées. Changer le mot de passe ne révoque pas les anciennes copies et ne rechiffre pas les objets. Les sauvegardes de Store n'incluent pas automatiquement les fichiers de clés externes.

## Préparer, inspecter et restaurer localement {#local}

Initialiser normalement les Stores source et destination en clair. Choisir le transport hors des Stores en clair, des dossiers de projets conventionnels `.syrnck` et des workspaces natifs enregistrés. Le CLI crée un Store de transport absent pour préparation/publication/réception et rouvre un Store existant. Il n'utilise pas de clé au repos pour ce cache : les corps d'objets sont déjà chiffrés. L'API C++ accepte des Stores de transport chiffrés indépendamment au repos.

```text
syrnck --store project/.syrnck sealed snapshot heads/main --wire private-cache/team-wire
syrnck --store project/.syrnck sealed inspect SEALED_ID --wire private-cache/team-wire
syrnck --store clone/.syrnck sealed open SEALED_ID --wire private-cache/team-wire --checkpoint sealed/checkpoints/manual
```

Ces chemins illustrent l'usage : `private-cache` doit être hors des deux projets. `snapshot` prend des noms de références locales de rétention explicites ; consulter `refs` pour les choisir. `--channel NOM` vaut `main` par défaut. Pour une préparation incrémentale, ajouter `--previous SEALED_ID` de même clé/canal. Un index Merkle opaque persistant réutilise le chiffré inchangé et met à jour les chemins d'index affectés. Des racines identiques rendent l'ID précédent sans nouveaux nonces.

L'inspection reste en lecture seule. L'ouverture vérifie hashes typés originaux et hashes complets des fichiers, importe les objets originaux, puis publie atomiquement des références immuables `sealed/roots/ID/NAME_HASH` et le point de contrôle choisi. Branches natives, staging et fichiers de travail restent en place. Lire le rapport privé `roots`, puis demander explicitement checkout/export natif ou reconstruction du manifeste. Les rapports contiennent noms/IDs originaux et restent côté client.

## Transférer via le serveur ordinaire {#remote}

Configurer les [identités et règles de branches](doc:server-access) d'un canal dédié. Les écrivains doivent accéder à l'espace `sealed` du support ; une ACL en clair `Content`/`Source` ne suffit pas. Configurer la branche publique du support en avance sans divergence, sans politique de verrous sur chemins en clair. Les [règles de signature](doc:server-signatures) facultatives authentifient supports/révisions d'index signés. Configurer l'identité/le signataire natif habituel du client avant préparation/publication si les signatures sont exigées. Cette identité devient publique sur le support.

```text
syrnck --store project/.syrnck sealed push https://blocks.example.invalid heads/main --wire private-cache/team-wire --require-lease
syrnck --store clone/.syrnck sealed fetch https://blocks.example.invalid --wire private-cache/clone-wire --require-read-lease
```

Fournir la configuration `SYRNCK_TOKEN`/CA existante. Parallélisme des requêtes, buffers, compression, débit et relances suivent les [options de transfert](doc:transfer-policy). Une publication observe d'abord une racine publique exacte, télécharge/authentifie son historique chiffré, contrôle le point local accepté, prépare le chiffré contre cette racine puis publie avec l'attente CAS initiale. Elle ne renouvelle pas cette attente après une course.

Par défaut, les révisions originales modifiées doivent avancer sans divergence. Supprimer/remplacer une racine ou modifier une racine non révisionnelle exige `--allow-rewrite`. Une révision divergente exige une fusion ou ce drapeau explicite. Celui-ci ne contourne pas les règles du support côté serveur. Le serveur ne peut inspecter ni imposer les ACL, verrous d'assets, signatures ou ancêtres originaux cachés ; les détenteurs de clé doivent appliquer leurs propres politiques en clair. Les déclarations de verrous en clair sont refusées par les transferts chiffrés.

## Reprendre l'opération exacte {#resume}

Les sessions de transfert sont dans le **Store de transport** :

```text
syrnck --store private-cache/team-wire agent sessions
syrnck --store project/.syrnck sealed resume https://blocks.example.invalid SESSION --wire private-cache/team-wire
```

Utiliser les mêmes clé cliente, Store source/destination et canal. Une publication inachevée bloque une nouvelle publication, évitant que de nouvelles enveloppes aléatoires masquent un résultat incertain. La reprise garde racine préparée et valeur distante attendue exactes. Une publication prouvée est enregistrée avant de terminer sa session ; un reçu historique terminé ne peut faire reculer un point de contrôle plus récent. Un téléchargement interrompu réutilise le chiffré vérifié et termine son import/point de contrôle initial. Ne pas oublier une session incertaine pour fabriquer une nouvelle attente.

Avant publication du point de contrôle, un import interrompu peut laisser des objets originaux valides sans référence. Un échec d'authentification ne peut publier une racine en clair. Racines de transport préparées et sessions actives protègent le chiffré du GC local. Le GC ordinaire refuse toujours certaines opérations actives selon ses règles existantes.

## Historique et fraîcheur {#history}

Le point automatique est nommé `sealed/checkpoints/ENDPOINT_FINGERPRINT/CHANNEL`. Après acceptation d'un snapshot, les suivants doivent en descendre via les engagements authentifiés de racine précédente. Une ancienne racine valide, une divergence ou la disparition d'un canal déjà accepté sont refusées. Cela protège un point conservé par le client, pas un premier clone sans racine connue. Un serveur peut retenir des données récentes, présenter des premiers historiques différents à différents clients ou refuser le service. Échange de racine initiale de confiance et comparaison des points entre appareils restent à réaliser.

La restauration historique locale explicite utilise un autre point, par exemple `sealed/checkpoints/historical`. Elle ne réinitialise pas le point distant automatique. Racines précédentes et index d'objets croissant conservent les anciennes versions chiffrées, même après réécriture de l'historique original. Ouvrir/importer le snapshot historique avant d'attendre ses racines originales dans un nouveau Store en clair. Ces octets restent retenus jusqu'à une future politique explicite de retrait de leurs racines protectrices. Les commandes actuelles ne compactent pas l'historique et ne renouvellent/révoquent pas les clés de domaine.

## Format et bornes de ressources {#format}

Les objets utilisent des nonces XChaCha20-Poly1305 aléatoires frais de 24 octets. BLAKE3 à clé dérive des clés indépendantes d'adresse/objet/racine depuis une clé maîtresse aléatoire. Une enveloppe `SYRSEA01` contient domaine public de 16 octets, nonce, chiffré et tag ; les données associées lient en-tête, domaine objet/racine et adresse opaque/canal. Le clair d'objet lie type et ID originaux. Le clair racine engage racines privées, support précédent, racine d'index et version/temps/identité du support public. Les nœuds Merkle visibles utilisent des révisions ordinaires validées ; les fichiers transportent le chiffré via le découpage ordinaire, y compris le surcoût d'un objet original de 16 Mio. Le chiffrement n'est ni déterministe ni convergent.

| Borne CLI | Défaut | Sens |
| --- | --- | --- |
| `--sealed-max-objects` | 4194304 | Entrées de fermeture originale |
| `--sealed-max-work` | 268435456 | Travail de parcours/index compté |
| `--sealed-cache-bytes` | 16777216 | Cache des nœuds d'index analysés ; nœuds trop gros lus sans cache |
| `--sealed-new-index-bytes` | 67108864 | Nouvelles adresses comptées avant chiffrement |
| `--sealed-max-history` | 100000 | Parcours des prédécesseurs du point accepté |

Un nœud contient au plus 256 entrées de feuille ou 16 liens enfants. Ces bornes ne représentent pas la RAM totale. Préparation/vérification auditent encore fermetures et octets complets ; seuls génération du chiffré, changements Merkle et corps d'objets transférés sont différentiels. Performances sur gros projets et workflows hors Windows restent à qualifier. L'[API C++](doc:api-sealed-repositories) existe ; bindings C/Go, intégration résidente/BEP, coffres OS, gestion des appareils, rotation/révocation distribuée, réparation automatique de répliques et rétention distribuée restent à terminer.
