---
id: syncthing
title: Adaptateur de blocs Syncthing
description: Guide : Adaptateur de blocs Syncthing.
section: API
order: 660
parent: integrations
sourceRevision: 5
---

# Adaptateur Syncthing à blocs fixes {#syncthing-fixed-block-adapter}

Ce module Go optionnel relie le C++ au modèle de blocs BEP via la bibliothèque Syncthing non modifiée **v2.1.7**, commit `d6d0181f7d46`, pseudo-version figée dans go.mod/go.sum. Le binding cœur reste séparé, sans dépendances Go externes. Le module demande Go 1.26.2+.

Le module gère données de fichiers ordinaires, connexions à appareils associés,
catalogue causal durable et contrôleur continu de workspace natif. `Folder`
capture les changements locaux, applique les versions entrantes vérifiées et
conserve l’état de reprise ; `FolderSession` le fait fonctionner sur BEP
authentifié. Découverte, interface d’appairage, exécutable de service autonome et
sémantique complète des dossiers Syncthing restent à terminer, selon les limites
précisées ci-dessous.

## Bibliothèque native et dépendances {#build-and-test}

Compiler `syrnck_ffi` et télécharger une fois les dépendances Go figées avec `go mod download` depuis `bindings/syncthing`. L’adaptateur est optionnel, séparé du binding cœur. Linux/macOS utilisent cgo et leur chemin de chargeur. Catalogue et transport précédents ont passé la CI Linux ; ce nouveau contrôleur est vérifié localement sous Windows, son exécution multiplateforme reste à qualifier. Ne distribuer que les dépendances et notices requises pour l’intégration choisie.

## API {#apis}

`NewFile(ctx,store,manifest,folder,name,blockSize)` crée une vue immuable servable. Il vérifie le hash complet sans exporter puis lit des plages vérifiées pour construire les `protocol.FileInfo` SHA-256. CDC/blocs fixes et IDs BLAKE3 du moteur restent intacts. Taille 0 : politique Syncthing ; sinon puissance de 2 de 128 Kio à 16 Mio. Index limité 65536 blocs. Fichier vide : bloc SHA-256 vide unique de référence.

`Info()` copie l’index indépendamment ; l’application y pose vecteurs de version, séquences et timestamps avant annonce. Noms relatifs UTF-8 valides : noms natifs Darwin NFD normalisés en NFC et séparateurs Windows convertis en `/` protocolaire. Index, requêtes de blocs et identité de reprise utilisent le même nom canonique.

`ReadRequest(ctx,req)` sert exactement un bloc annoncé pour ce folder/fichier. Un SHA-256 fourni doit correspondre ; hash optionnel et ancien numéro de bloc 0/défaut sont acceptés. Une requête temporaire peut revenir à cette source immuable complète. Autres chemins/plages/générations refusés. Le payload est vérifié par le moteur puis par SHA-256 avant retour.

`Fetch(ctx,destination,folder,info,request,options)` reconstruit un fichier annoncé. Fournir `protocol.Connection.Request` ou un autre demandeur authentifié. Chaque réponse doit avoir taille/hash exacts. `ReuseManifest` indexe une version locale à la taille reçue pour réutiliser blocs identiques, déplacés ou répétés ; seuls les absents sont demandés. `FetchResult.Stats` sépare octets demandés, réemploi local et staging vérifié.

Après assemblage/vérification privée, Fetch ingère le fichier complet via ABI C avec blocs fixes BEP et recontrôle l’index stocké contre les SHA-256 reçus. `ffi/ID` retient le manifeste. L’application peut restaurer exclusivement ou l’inclure dans sa propre transaction ; Fetch ne remplace ni fichier de travail ni branche mutable.

`syrnck_file_verify`/`Store.VerifyFile` vérifient fichier complet, blocs, tailles et file_hash, sans destination ni référence, en buffers natifs bornés. Une bibliothèque reconstruite avec ce symbole est nécessaire.

```go
view, err := syncthing.NewFile(ctx, source, manifest, "project", "Content/Map.uasset", 0)
// Check err, authenticate the peer and announce view.Info() through its model.
result, err := syncthing.Fetch(ctx, destination, "project", incomingInfo,
    connection.Request, syncthing.FetchOptions{
        ReuseManifest: previousManifest,
        ResumeDirectory: perContentPrivateDirectory,
    })
// Check err before using result.Manifest or publishing a workspace revision.
```


## Catalogue persistant de dossier {#folder-catalog}

`OpenCatalog(ctx, store, CatalogOptions)` charge le catalogue persistant ou crée
un brouillon vide non publié. Associer le dossier à l’ID complet de l’appareil
local et à la liste exacte des pairs de confiance. Doublons, appareil local,
sentinelles et collisions des identités 64 bits des vecteurs sont refusés.
Changer la liste d’options ne modifie pas implicitement les autorisations.
Une procédure distincte de gestion des membres reste à réaliser.

`Catalog` ne garde ni Store ni connexion. `LocalIndex` et `PeerIndex` rendent des
copies indépendantes. `RecordLocal` ou `RecordLocalChanges` attribue les séquences
locales et avance le vecteur local antérieur avec contrôle de débordement ; les
champs causaux fournis doivent être vides. Un lot est validé entièrement avec un
seul tri des noms. Les suppressions doivent être des tombstones explicites.
Dossiers et liens symboliques sont conservés comme métadonnées ; aucune création
de dossier ni résolution de cible de lien n’est faite par cette API.

`ReceiveIndex(CatalogIndex)` conserve un Index complet ou un delta d’un pair.
L’appelant doit lier `Device` à la connexion authentifiée, autoriser le dossier
et prendre `IndexID` dans son ClusterConfig négocié. Une nouvelle génération
exige un index complet ; les lots suivants continuent la séquence persistée.
Noms et séquences uniques, vecteurs triés et bornés : compteurs dupliqués,
flags invalides, entrées chiffrées non prises en charge et blocs mal formés
sont refusés. Un vecteur identique ne peut décrire d’autres métadonnées de
contenu. Recevoir une annonce ne la transforme pas en fichier local et ne
lance aucun téléchargement.

`Attach(ctx, store, device, name, manifest)` vérifie les données contre les blocs
de l’entrée actuellement enregistrée. `Adopt(device, name)` promeut une entrée
vérifiée causalement plus récente dans l’index local annoncé. Une concurrence
rend `ErrCatalogConflict` ; versions antérieures et contenu incohérent à vecteur
identique sont refusés. Les deux contenus restent conservés sans gagnant
implicite. Seul le catalogue annoncé change : l’application au workspace doit
avoir sa transaction de reprise avant qu’un service annonce un dossier à jour.

`Commit(ctx, store)` vérifie les données retenues puis publie état et données
sous une racine unique `ffi/bep/…` de révision native protégée par valeur attendue.
Les catalogues précédents sont parents natifs : reset d’index, renommage et
suppression conservent donc les versions complètes au GC et en sauvegarde.
Les chemins de payload sont les hashes des identités appareil/nom : noms
`.git/config`, distincts par la casse et incompatibles avec un workspace portable
restent stockables sans perte. Cette API ne les matérialise pas. Aucune racine
implicite `ffi/manifestID` n’est ajoutée.

Un brouillon périmé rend `ErrCatalogStale` : recharger et replanifier sans
recibler l’opération ancienne. Une erreur d’IO de publication laisse un résultat
inconnu ; recharger et établir le résultat persistant avant toute reprise.
Un commit inchangé ne fait rien. Le nouvel IndexID local devient persistant au
premier commit réussi. Le codec refuse champs dupliqués/inconnus et état non
canonique. Limites : 256 pairs, 256 compteurs/vecteur, 65536 entrées au total et
16 Mio de métadonnées. Chaque commit vérifie actuellement fichiers retenus et
fermeture native ; débit sur grand projet non établi. Retrait de l’historique
et partitionnement du catalogue restent à réaliser.

## Télécharger sans garder le Store ouvert {#staged-download}

`DownloadFile(ctx, folder, info, request, DownloadOptions)` vérifie les blocs
réseau dans un staging privé sans ouvrir le Store natif. `ReuseIndex` et
`ReadReuse` réutilisent les blocs locaux identiques ; le callback peut ouvrir
et fermer brièvement un Store. Des tailles de blocs différentes désactivent
le réemploi. La reprise utilise les mêmes descripteurs et contrôles que Fetch.

Le `StagedFile` retourné ne garde ni fichier ouvert ni Store. Toujours appeler
`Close()`. `Info()` copie indépendamment ; `Stats()` décrit transfert, réemploi
et staging. Appeler `Prepare(ctx, store)` en possédant brièvement le Store :
import sans racine implicite et recontrôle contre les blocs annoncés. Attacher
le manifeste à la version courante exacte du pair, l’adopter si nécessaire et
faire Commit avant de fermer le Store ou d’autoriser le GC. L’attente réseau ne
garde ainsi pas le verrou exclusif natif. Revérifier l’entrée persistée après
transfert, car elle peut avoir avancé pendant le téléchargement.

`PrepareFetch` simplifie l’appel si un Store est déjà ouvert, avec la même règle
de rétention explicite. `Fetch` garde sa rétention indépendante historique.
Ne pas supprimer une racine partagée historique appartenant à un autre appelant.

## Authentifier une connexion d’appareil {#device-connections}

`OpenDeviceConnection(ctx,stream,DeviceOptions,model)` prend possession d’un
`net.Conn` connecté et non chiffré, y compris en cas d’échec. Fournir un certificat
TLS local unique, sa clé privée `crypto.Signer` correspondante et le
`protocol.DeviceID` du pair explicitement associé. `Server: true` sélectionne le
côté acceptant. Auto-connexion, identités vides/réservées, clés malformées et
discordance certificat/clé sont refusées. Ne modifier ni certificat ni clé en cours d’utilisation.

TLS 1.3 ou ultérieur et ALPN `bep/1.0` sont exigés. Le Hello amont borné est
échangé avant de rejeter un pair non associé, puis l’identité SHA-256 de son
unique certificat est comparée exactement. C’est un épinglage de certificat :
DNS, CA Web PKI, expiration, découverte et nom Hello n’accordent pas l’accès.
`PeerHello` contient des descriptions non fiables. Aucun modèle applicatif
n’est attaché avant la vérification réussie.

```go
session, err := syncthing.OpenDeviceConnection(ctx, stream, syncthing.DeviceOptions{
    Certificate: deviceCertificate,
    RemoteDevice: pairedDeviceID,
    Hello: protocol.Hello{DeviceName: "Asset workstation"},
}, folderModel)
// Traiter err avant utilisation. Démarrage explicite, puis ClusterConfig comme
// premier message après authentification. Le modèle autorise chaque dossier.
session.Start()
session.ClusterConfig(clusterConfig, nil)
// Envoyer un Index complet avant les mises à jour, sauf accord persistant
// d’index delta. Fetch peut utiliser session.Request pour les blocs vérifiés.
```

`HandshakeTimeout` vaut 20 secondes par défaut et accepte de 100 ms à une minute.
Il couvre TLS et Hello ; une échéance du contexte plus proche l’emporte. La
compression des métadonnées amont est le défaut ; Never/Always sont acceptés.
Le contexte contrôle toute la session, même authentifiée mais pas encore démarrée.
`Close(reason)` libère le flux ; attendre le callback `Closed` du modèle avant
de fermer les handles natifs qu’utilisent ses callbacks. Les métadonnées de
connexion gardent une date d’établissement et un ID de session stables.

L’application fournit stockage/rotation des certificats, Device ID de confiance,
planification, transactions du catalogue, reprise du workspace et résolution
des conflits. Le chiffrement du transport protège et
authentifie ce pair ; un daemon recevant des données ordinaires ne devient pas
une sauvegarde chiffrée non fiable. Le parcours complet de chiffrement vers un
appareil non fiable reste à intégrer.

## Vérifier avec le daemon de référence {#reference-daemon}

```text
python scripts/build_syncthing_reference.py --download
```

Ce helper de test facultatif compile `cmd/syncthing` à la version exacte du module
de l’adaptateur avec les manifests figés `tests/syncthing-reference/go.mod` et
`go.sum`. Le tag amont `noassets` omet les ressources d’interface web et conserve
daemon réel, base de données, scanner, modèle, puller et API REST. La sortie est
`build/qualification/syncthing` (`.exe` sous Windows). Sans `--download`, le cache
de modules doit être prêt : aucun téléchargement implicite. La version amont
embarquée est contrôlée ; version différente ou manifests modifiés sont refusés.

Définir `SYRNCK_SYNCTHING_BINARY` avec son chemin absolu puis exécuter :

```text
python scripts/test_go.py --library build/Release/syrnck_ffi.dll --module syncthing --check-pointers
```

Sous Linux/macOS, fournir le chemin de la bibliothèque partagée construite. La
CI des trois plateformes active explicitement l’adaptateur et ce test. Le daemon
utilise configuration, clés et dossier neufs propres au test, écoute uniquement
en loopback, désactive découverte/relais/NAT/mises à jour/rapports et est terminé
après le test. Aucun profil Syncthing installé n’est lu ou modifié.

La vérification locale Windows a exercé les deux sens contre le daemon amont
non modifié : un asset de 384 Kio à blocs déplacés/réutilisés transfère uniquement
le bloc absent de 128 Kio dans chaque sens. La reconstruction dans un stockage
Syrnck chiffré vérifie le fichier complet. Les annonces réelles de renommage et
tombstone ont aussi été reçues. Cela prouve transport et interopérabilité des
fichiers ordinaires. Un autre test réel conserve index complets/deltas et données,
ferme les Stores pendant l’attente réseau puis se reconnecte après un renommage
hors ligne avec tombstone persistant. Le service autonome reste à réaliser.
Sans binaire explicite, ce test annonce qu’il est ignoré ; les tests ordinaires
de l’adaptateur continuent de s’exécuter.

## Reprise et annulation {#resume-and-cancellation}

Sans `ResumeDirectory`, Fetch utilise un fichier temporaire privé retiré au retour. Avec cette option, le dossier par contenu appartient à l’appelant : `state.json`, `data`. Le descripteur lie folder/nom/taille/taille de bloc et hash de liste complète. Autre index, fichier inconnu, descripteur incomplet ou lien est refusé. Utiliser un dossier privé et sérialiser ses gestionnaires.

Chaque plage de staging est rehashée au retry. Seules les plages complètes conformes sont réutilisées ; partielles/corrompues redemandées. Les trous remplis de zéro peuvent satisfaire un bloc zéro sans requête. Flush tous les 16 blocs et avant ingestion. Annulation préserve le staging fourni par l’appelant, qui reste **en clair même avec Store chiffré** ; protéger l’emplacement. Le retrait des dossiers terminés est explicite.

Le contexte est vérifié entre appels natifs/étapes réseau/blocs. La préparation sans racine implicite propage l’annulation dans l’ingestion native ; l’ingestion retenue historique et la vérification complète terminent leur appel natif synchrone courant. Une annulation tardive ou changement détecté peut laisser un manifeste complet retenu, dont l’ID accompagne l’erreur. Traiter l’erreur avant publication. Aucun fichier de travail partiel installé. Un kill peut laisser du privé ; durabilité après coupure et retrait automatique ne sont pas établis.

## Contrôleur continu de dossier de travail {#folder-controller}

Créer d’abord Store et workspace natifs puis appeler `NewFolder(ctx, options)`.
`FolderOptions` lie catalogue, Device ID local, pairs explicites, dossier, Store
et contexte enregistré facultatif. `OpenStore` retourne un nouveau handle que le
contrôleur possède et ferme ; une intégration chiffrée obtient sa clé auprès du
fournisseur de l’application. Aucun Store n’est gardé au repos. Fermer les sessions
avant Folder.

`Refresh` capture les changements avec watcher/index natifs et audits complets
périodiques (cinq minutes par défaut). `Invalidate` demande un audit immédiat.
`ReceiveIndex` conserve l’index complet/delta d’un appareil associé authentifié.
`Step` télécharge un fichier manquant, réutilise les blocs vérifiés des fichiers
locaux, puis applique le candidat complet par checkout natif à propriétaire.
Un renommage exact ne transfère aucun payload. Les octets immuables annoncés
restent servables pendant le téléchargement ; un worker séparé reçoit les index.
Génération et descripteur téléchargé sont recontrôlés avant attachement : une
réponse périmée ne peut pas être installée.

Trois racines typées conservent descripteur, candidat natif et catalogue préparé
avant l’application. La publication protocolaire attend la preuve native de fin
avec propriétaire durable unique, source, cible et branche sélectionnée exacts.
`CheckoutStatusContext` utilise le décodeur de reçus du cœur, sans réimplémenter
son format privé en Go ni comparer tous les arbres historiques d’assets. Une
égalité de HEAD seule ne suffit pas. Une édition après fin native est capturée
avec la base causale reçue, sans blocage de l’accusé ni écrasement. Une préparation
obstruée avant tout journal natif est retirée uniquement après vérification de
l’état natif original exact. Les journaux étrangers ou ambigus restent explicites.

Les parents des révisions du catalogue conservent les versions complètes ; les
candidats en attente sont aussi des racines de GC. Les tests terminent brutalement
un vrai processus avant/après publication native, exécutent le GC, rouvrent,
reprennent et vérifient les données reçues ou leur édition locale ultérieure,
avec Stores chiffrés et non chiffrés. Les versions protocolaires concurrentes
bloquent actuellement ; les copies de conflit automatiques de ce contrôleur BEP
restent à réaliser.

## Sessions authentifiées continues {#folder-sessions}

`OpenFolderSession(ctx, folder, stream, options)` possède le transport connecté,
vérifie le Device ID local complet du certificat, authentifie le pair configuré,
puis négocie générations et index complets/deltas du dossier. `PollInterval` vaut
250 ms par défaut (50 ms à une minute). La file entrante vaut 32 Mio par défaut
(1 à 128 Mio), inclut le batch en traitement et ne bloque jamais le lecteur BEP
sur le travail disque natif. `Reports`, `Errors` et `Done` exposent progression,
blocages et fin. `Close` arrête les workers et attend transport/callback de fin.

L’application intégratrice gère écoute/connexion, délai de reconnexion,
certificats/clés et cycle de vie de service OS. Ce module est séparé de la
distribution CMake du moteur. Le contrôleur applique les fichiers ordinaires et
leurs tombstones, pas les métadonnées de dossiers/liens symboliques, dossiers
vides, propriétaires/xattrs ni dossiers receive-encrypted. Les métadonnées
entrantes sont conservées, sans prétendre que ces éléments ont été appliqués.
Copies de conflit, reprise persistante de téléchargement, gestion automatique
des membres/retrait d’historique et politique d’auteur/signature des nouveaux
candidats natifs entrants restent à réaliser. Performances à grande échelle et
durabilité face à une coupure électrique ne sont pas qualifiées. L’agent natif
résident de publication est une autre fonctionnalité.

`StagedFile.PrepareWithChunking(ctx, store, syrnck.Chunking{})` conserve les blocs
FastCDC natifs par défaut indépendamment des frontières SHA-256 BEP. `Prepare`
garde ses blocs fixes BEP. `Catalog.CommitWithRefs` publie atomiquement catalogue
et gardes de références typées additionnelles ; recharger après un résultat de
publication inconnu, selon les règles du CAS natif.

Une édition externe pendant installation/vérification d’un journal natif actif
bloque la reprise pour ne pas écraser ce travail. Conserver les données modifiées
et résoudre explicitement le journal ; préservation/réconciliation automatique
pendant l’application active reste à terminer. La visibilité du fichier seule ne
signale pas la fin : utiliser le checkpoint/statut du contrôleur. C’est distinct
de l’édition prise en charge après le reçu natif committed, y compris avant son
accusé de réception protocolaire.

Le contrôleur associe l’orthographe native aux noms NFC du protocole, conserve
l’orthographe décomposée existante lors des éditions et refuse deux noms natifs
qui se normalisent vers le même chemin. Écho et nouveau scan ne transforment
pas cette différence d’orthographe en suppression.
