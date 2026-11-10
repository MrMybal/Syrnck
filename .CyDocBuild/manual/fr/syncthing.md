---
id: syncthing
title: Adaptateur de blocs Syncthing
description: Guide : Adaptateur de blocs Syncthing.
section: API
order: 660
parent: integrations
sourceRevision: 16
---

# Adaptateur Syncthing à blocs fixes {#syncthing-fixed-block-adapter}

Ce module Go optionnel relie le C++ au modèle de blocs BEP via la bibliothèque Syncthing non modifiée **v2.1.7**, commit `d6d0181f7d46`, pseudo-version figée dans go.mod/go.sum. Le binding cœur reste séparé, sans dépendances Go externes. Le module demande Go 1.26.2+.

Le module gère données de fichiers ordinaires, connexions à appareils associés,
catalogue causal durable et contrôleur continu de workspace natif. `Folder`
capture les changements locaux, applique les versions entrantes vérifiées et
conserve l’état de reprise ; `FolderSession` le fait fonctionner sur BEP
authentifié. Découverte, interface d’appairage, installation du service OS et
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
une sauvegarde chiffrée non fiable. Le [mode facultatif pour appareil non fiable](doc:syncthing#untrusted) ajoute le chiffrement de bout en bout vers un pair receive-encrypted.

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
hors ligne avec tombstone persistant. Le [service autonome](doc:syncthing#standalone-service) est disponible pour les fichiers ordinaires.
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
bloquent par défaut ; la [politique explicite de copies](doc:syncthing#protocol-conflicts)
conserve les deux contenus.

## Téléchargements persistants du contrôleur {#persistent-downloads}

`FolderOptions.ResumeDownloads: true` réutilise les plages privées vérifiées
après perte de connexion et redémarrage du contrôleur/processus. L’équivalent
autonome est `"resume_downloads": true` ; le défaut vaut false. Une intégration
doit fournir explicitement le dossier du Store. Le chemin optionnel reste sous
`STORE/bep-downloads/v1/`, groupé par identité du dossier/workspace, avec un hash
du pair épinglé, de la génération et de l’index exact du contenu annoncé.
Une autre génération ou un autre contenu choisit un chemin distinct.

Un verrou système sérialise le staging du dossier entre contrôleurs, sans Store
natif ouvert pendant l’attente réseau. Une contention rend
`ErrFolderDownloadBusy` ; terminer le processus libère le verrou. Son fichier
reste en place pour conserver son identité. Windows, Linux et macOS possèdent
une implémentation ; les autres cibles non prises en charge refusent cette
option. Ne pas supprimer ni modifier ces chemins pendant l’exécution.

Chaque plage sauvegardée est rehashée contre le SHA-256 annoncé avant réemploi.
Un bloc incomplet ou corrompu est redemandé. L’import vérifie le flux complet. L’attachement exige génération et métadonnées
exactes toujours annoncées avant rétention du manifeste immuable. Aucun téléchargement périmé n’est installé.
Le couple de staging complet correspondant est retiré uniquement après
rétention réussie du catalogue ; l’application aux fichiers de travail utilise
toujours sa transaction native à propriétaire et son reçu.

Le staging persistant est **en clair même avec des objets du Store chiffrés**.
Garder le dossier du Store privé et activer l’option volontairement. L’exemple la
laisse désactivée. Les sauvegardes du moteur n’incluent pas ces plages privées
non retenues. Le staging échoué ou obsolète reste disponible pour reprise ou
inspection ; retrait automatique selon âge/quota reste à finir. Après arrêt de
tous les contrôleurs, ces dossiers peuvent être retirés ; ce ne sont pas des
racines d’historique. Entrées inconnues ou métadonnées différentes sont refusées,
sans suppression récursive. Un nettoyage interrompu peut laisser un dossier
privé incomplet à inspecter.

Les tests couvrent réouverture, corruption, contention du verrou, génération
distante modifiée et annonces périmées. De vrais processus terminés brutalement
dans des Stores clairs/chiffrés gardent le premier bloc vérifié et redemandent
seulement les autres au redémarrage. Les tests autonomes face au daemon amont
activent cette option pour changements dans les deux sens, renommage hors ligne
et conflits. Cela ne prouve ni durabilité après coupure ni résistance à tout
écrivain externe.

## Versions protocolaires concurrentes {#protocol-conflicts}

Activer `FolderOptions.ConflictCopies: true`, ou `"conflict_copies": true` dans
la configuration autonome, pour résoudre les vecteurs concurrents de fichiers
ordinaires. Le défaut strict rend toujours `ErrCatalogConflict` ; `Catalog.Adopt`
garde ce comportement. Une intégration peut appeler explicitement
`Catalog.ResolveConcurrent(ctx, store, peer, name)` sur un brouillon, puis le
publier par la transaction vérifiée habituelle. Le résultat décrit le choix du
brouillon, pas un reçu d’application aux fichiers de travail.

Les deux contenus vivants sont vérifiés contre leurs blocs BEP annoncés avant
arbitrage. `FileInfo.WinsConflict` de la version amont figée choisit la date de
modification la plus récente, puis ordonne les vecteurs concurrents en cas
d’égalité. Une version capturée localement utilise le timestamp de capture
native. Le gagnant conserve ses métadonnées et reçoit l’union des deux vecteurs.
Des contenus concurrents identiques sont regroupés par hash natif du fichier
complet, indépendamment du découpage ; deux tombstones sont aussi regroupés.

Le contenu perdant distinct devient une copie voisine suivie avec `.sync-conflict-`,
une identité déterministe origine/vecteur/contenu et l’extension courte conservée.
Une suppression perdante n’a rien à copier ; une suppression gagnante garde le
fichier vivant perdant dans une copie. Les noms suivis et tombstones existants
sont réservés, avec casse/normalisation Unicode et collisions de préfixes
fichier/dossier. Des variantes bornées évitent d’écraser un chemin suivi occupé.
Les autres obstacles natifs de structure ou de fichiers de travail bloquent.
Les copies respectent la sélection matérialisée ; une copie masquée reste un
fichier logique retenu. Il n’y a pas de fusion de contenu texte ou binaire.

Gagnant, copie et causalité sont figés dans la transaction native en attente.
La reprise applique ce candidat même après désactivation de l’option. Seul son
reçu de fin à propriétaire autorise l’acquittement du catalogue. Les deux entrées
restent accessibles dans l’historique ; les copies sont des fichiers suivis sans
retrait automatique. Syncthing peut créer ses propres noms de copie s’il résout
le conflit en premier.

Les tests couvrent convergence symétrique de deux contrôleurs sans historique
répété, contenus identiques, combinaisons vivant/supprimé, refus de données
invalides et collisions Unicode. Arrêts forcés avant/après publication, GC,
sauvegarde/restauration vérifiées et reprise gardent les deux entrées dans des
Stores clairs/chiffrés. Les processus autonomes conservent aussi le perdant sur
les deux appareils face au daemon amont non modifié, avec chaque côté gagnant.

## Sessions authentifiées continues {#folder-sessions}

`OpenFolderSession(ctx, folder, stream, options)` possède le transport connecté,
vérifie le Device ID local complet du certificat, authentifie le pair configuré,
puis négocie générations et index complets/deltas du dossier. `PollInterval` vaut
250 ms par défaut (50 ms à une minute). La file entrante vaut 32 Mio par défaut
(1 à 128 Mio), inclut le batch en traitement et ne bloque jamais le lecteur BEP
sur le travail disque natif. `Reports`, `Errors` et `Done` exposent progression,
blocages et fin. `Close` arrête les workers et attend transport/callback de fin.

L’application fournit écoute/connexion, certificats/clés et cycle de vie OS.
Le [service résident](doc:syncthing#folder-service) gère attente de reconnexion
et capture hors ligne indépendante. Ce module est optionnel ; la cible BEP CMake distribue l’exécutable autonome. Le contrôleur applique les fichiers ordinaires et
leurs tombstones, pas les métadonnées de dossiers/liens symboliques, dossiers
vides ni propriétaires/xattrs. Le [récepteur opaque](doc:syncthing#opaque-receiver) distinct conserve les métadonnées chiffrées sans les appliquer à un workspace. Les métadonnées
entrantes sont conservées, sans prétendre que ces éléments ont été appliqués.
Gestion automatique
des membres/retrait d’historique reste à réaliser. Performances à grande échelle et
durabilité face à une coupure électrique ne sont pas qualifiées. L’agent natif
résident de publication est une autre fonctionnalité.

`StagedFile.PrepareWithChunking(ctx, store, syrnck.Chunking{})` conserve les blocs
FastCDC natifs par défaut indépendamment des frontières SHA-256 BEP. `Prepare`
garde ses blocs fixes BEP. `Catalog.CommitWithRefs` publie atomiquement catalogue
et gardes de références typées additionnelles ; recharger après un résultat de
publication inconnu, selon les règles du CAS natif.

Une édition externe pendant installation native reste bloquée par la reprise
standard. Activer `FolderOptions.PreserveCheckoutEdits = true` pour conserver
les éditions de fichiers ordinaires selon la politique ci-dessous. La visibilité
du fichier seule ne signale pas la fin : utiliser checkpoint/statut. Une édition
après le reçu natif committed reste prise en charge indépendamment de cette option.

Le contrôleur associe l’orthographe native aux noms NFC du protocole, conserve
l’orthographe décomposée existante lors des éditions et refuse deux noms natifs
qui se normalisent vers le même chemin. Écho et nouveau scan ne transforment
pas cette différence d’orthographe en suppression.

## Conserver les éditions pendant le checkout actif {#active-checkout-edits}

L’option explicite `PreserveCheckoutEdits` est figée dans chaque application
préparée ; une nouvelle configuration ne peut pas recibler l’opération interrompue.
La reprise vérifie propriétaire, cible, contexte, dossier et fichiers privés de
staging/sauvegarde exacts. Elle ingère les fichiers ordinaires modifiés dans des
révisions d’archive avant tout déplacement. Déplacement exclusif vers une copie
de conflit et restauration exclusive des octets intermédiaires attendus sont
rejouables depuis leurs descripteurs retenus. Le checkout strict original
revérifie ensuite tous les fichiers cibles et publie son candidat/reçu inchangés.

Les racines `archive/checkout-edits/TRANSACTION/EDIT/state` (descripteur) et
`/work` (révision) conservent les versions ; les parents natifs gardent la révision
entrante et ses fichiers complets. Les racines `archive/checkout-edit-versions/TRANSACTION/REVISION` retiennent les éditions
supplémentaires d’une copie pendant la reprise. Chaque nom est déterminé par
l’identité persistée de l’édition, conserve les extensions courtes et n’est jamais
écrasé. Un fichier apparu au nom réservé est conservé ; les éditions restantes de
l’original reçoivent une nouvelle identité. `preserved_edits` rend chemins source,
copie et IDs de révisions archivées ; les octets actuels de la copie peuvent être
plus récents que cette révision.

Après l’accusé, la capture locale ordinaire annonce les fichiers conservés.
Cela protège les éditions de contenu avant sauvegarde, pendant staging et après
installation, y compris sur un fichier suivi normalement inchangé. La cible signée
préparée reste identique ; aucune clé privée de signature n’est requise pour la
reprise. Les archives restent des racines de GC après les changements du workspace.
Le retrait automatique de ces groupes demande encore une politique administrative ;
ne pas retirer leurs racines indépendamment.

Un éditeur qui continue à écrire peut différer la fin : chaque tentative bornée
garde ses observations pour la suivante. Intentions de suppression/fichiers
manquants, obstructions de dossiers/liens et structures ambiguës bloquent encore
plutôt que d’inventer une résolution. Cette option ne résout pas les vecteurs BEP
concurrents, ne fournit pas un service de dossiers complet et ne prouve pas la
durabilité face à une coupure électrique.

Si le checkout doit retirer ou renommer un dossier parent, la copie est placée
hors de ce dossier en gardant le nom de base et l’extension courte. Cela couvre
aussi dossier vers fichier et changement de casse d’un dossier.

## Service résident de reconnexion {#folder-service}

`OpenFolderService(ctx, folder, options)` maintient un pair explicitement appairé
malgré les coupures. Fournir `FolderServiceOptions.Session` avec certificat,
Device ID distant et budgets, puis `Connect(ctx)` retournant un `net.Conn`
connecté. Cette fabrique peut connecter ou accepter, doit respecter l’annulation
et transfère tout transport retourné, même accompagné d’une erreur. TLS/BEP
vérifie ensuite le pair ; la fabrique ne peut pas autoriser un appareil.

Le service observe/capture les modifications locales indépendamment du réseau :
éditions hors ligne, suppression/renommage et pair authentifié qui n’envoie
jamais sa configuration de dossier. `OfflineInterval` vaut 250 ms par défaut
(50 ms à une minute). Il fonctionne aussi pendant les téléchargements : le
contrôleur natif sérialise l’état et refuse les candidats périmés sans conserver
le Store pendant les attentes réseau. Les audits complets restent programmés.

`RetryMin` vaut 500 ms et `RetryMax` 30 secondes par défaut. L’attente exponentielle
reste entre les bornes configurées (50 ms à une minute) ; une connexion
authentifiée la réinitialise. Chaque reconnexion utilise un nouveau timestamp
Hello par défaut et reprend l’état natif/catalogue durable. Aucune découverte
ni appairage implicite ne sont ajoutés.

`Events` expose connecting, connected et retrying ; connected signifie certificat
authentifié, pas application du dossier terminée. `Reports` et `Errors` rendent
progression et échecs. Ces trois flux bornés sont consultatifs et peuvent perdre
des messages si le consommateur ralentit ; relire l’état durable pour les reçus.
`Close` annule et attend connexion, TLS et sessions, y compris un transport
terminant après annulation, puis ferme flux et `Done`. Arrêter le service avant
son Folder, qui appartient à l’appelant. Aucun Store n’est gardé au repos. Le
fournisseur du Folder contrôle la durée de vie des clés de stockage ; garder la
clé de signature d’appareil immuable jusqu’à l’arrêt. Close rend les erreurs de
fermeture des sessions.

Les tests couvrent deux workspaces clairs/chiffrés : capture hors ligne,
reconnexion TCP, renommage/tombstone, fermeture des transports échoués,
annulation TLS bloquée et capture locale sans configuration distante. Les tests
de session qualifient séparément le daemon amont figé. L’application fournit
encore écoute/connexion réseau, certificats, stockage des clés et cycle de vie OS.
Installation de service OS et métadonnées complètes des dossiers/liens
restent à terminer.

## Auteur natif et politique de signature {#native-authorship}

Configurer chaque Store rendu par `FolderOptions.OpenStore` avec l’[API d’identité](doc:go-binding#native-authorship).
Captures locales et révisions natives entrantes synthétisées utilisent alors
cet auteur/committer et son signataire facultatif. Une version BEP entrante
devient une révision d’intégration native locale, sans prétendre reprendre
l’auteur natif du pair. Le suivi interne du catalogue garde ses métadonnées v1.
Clés TLS d’appareil, clés de stockage et graines de signature ont des rôles
distincts. Les vecteurs de fichiers BEP n’importent pas les signatures natives.

Le JSON autonome accepte `author` facultatif : `name`, `email` et `timezone_offset`
(minutes vers l’est de UTC). Sans auteur, le comportement v1 reste. Avec auteur
sans `signing_key_env`, captures/candidats sont en v2 non signée. `signing_key_env`
nomme une variable majuscule distincte contenant une graine Ed25519 de 32 octets
sous forme de 64 caractères hexadécimaux, pour exiger des signatures. Aucune graine
brute en JSON ni arguments. Chaque Store court reçoit sa copie de clé native ;
les buffers Go décodés sont effacés immédiatement ensuite.

Une variable de signature configurée mais absente permet la reprise publique
d’un candidat signé déjà préparé, puis bloque les nouvelles révisions jusqu’au
retour de la graine. Une graine mal formée échoue. Les éditions restent en place
si la signature échoue. Candidat, parents et reçus natifs restent figés ; aucune
nouvelle signature pendant la reprise. Retirer l’exigence dans la configuration
est un changement de politique explicite. Coffres OS, distribution des secrets
et autorisation des signataires restent externes ; signature valide ne vaut pas
permission serveur.

Les tests terminent brutalement les applications signées avant/après publication
dans des Stores clairs/chiffrés, reprennent sans clé privée, refusent un repli non
signé sur l’édition suivante, puis reprennent les signatures avec le fournisseur
restauré. Redémarrages de processus autonomes et tests contre le daemon réel
qualifient captures locales et révisions natives entrantes signées. Le daemon
amont ne signe pas lui-même les révisions natives Syrnck.

## Exécutable autonome au premier plan {#standalone-service}

L’exécutable optionnel `syrnck-bep` maintient le service résident pour un workspace
existant et un pair explicitement associé. Il capture hors ligne, authentifie le
pair, se reconnecte et applique les versions ordinaires contrôlées. Aucun processus
Git ni installation Go ne sont nécessaires à l’exécution. Le moteur C++ conserve
stockage et révisions.

```text
syrnck-bep device-id --certificate device/cert.pem
syrnck-bep run --config service.json
syrnck-bep --lang fr run --config service.json
```

Fournir un certificat et une clé privée d’appareil distincts, puis autoriser le
Device ID complet affiché chez le pair. Une identité créée par `syncthing generate`
convient ; conserver ensemble certificat et clé correspondante. Une connexion à
soi-même est refusée. Le CLI importe les identités ; génération, rotation et
coffre système restent des travaux distincts. L’ID de dossier doit être commun.

Copier `share/Syrnck/examples/bep-service.json` installé dans le dossier de
configuration puis adapter chemins, pair et adresse. L’exemple existe aussi dans
`examples` des sources. Initialiser d’abord le Store avec le CLI `syrnck` principal.
Les chemins relatifs partent du fichier de configuration. Le Store vaut
`directory/.syrnck` par défaut ; `workspace` choisit un contexte lié existant.
`mode: dial` se connecte à `address` ; `listen` y accepte une connexion. L’adresse
`hôte:port` est explicite. Chaque processus possède transport et Folder.

La clé PEM privée doit rester hors du dossier de travail ou dans son Store
protégé, après résolution des liens. Elle ne doit pas devenir du contenu source.
`key_env` nomme la variable de clé de stockage (`SYRNCK_KEY` par défaut) : absente,
aucune clé n’est fournie ; sinon, 64 caractères hexadécimaux doivent correspondre
au Store existant. Aucun secret brut dans JSON ou arguments. Identité TLS et clé
de chiffrement au repos ont des rôles différents. Le fichier PEM reste protégé
par les droits du système de fichiers ; le chiffrement concerne les objets du Store.

Autre possibilité : fournir un [fichier de clé protégé](doc:key-files) avec
`"key_file": "storage.syrkey"` et, facultativement,
`"key_password_env": "SYRNCK_KEY_PASSWORD"`. Les chemins relatifs partent du dossier
contenant le JSON. La variable de mot de passe contient 1..4096 octets exacts
(UTF-8 depuis l'environnement Windows) et le `key_env` brut configuré doit être
vide. Aucun mot de passe dans JSON ou arguments. Le fichier est une enveloppe de
104 octets ordinaire, sans lien, hors du workspace ou dans son Store protégé.
L'échec d'authentification arrête avant capture/écoute, sans repli sur un Store
clair. Le propriétaire natif est réutilisé jusqu'à l'arrêt de tous les workers,
sans exporter la clé en clair vers Go. Tout changement d'enveloppe/mot de passe
requiert un redémarrage : réenvelopper la même clé dans un nouveau fichier et
mettre à jour configuration/identifiants avant de relancer. Un service actif
garde sa clé déjà authentifiée. Cela ne révoque pas les anciennes enveloppes.

La configuration version 1 refuse doublons/champs inconnus et plus de 64 Kio.
`poll_ms` vaut 50..60000 (zéro choisit 250) ; les délais de reconnexion valent
500/30000 ms par défaut. `use_permissions` et `preserve_checkout_edits` valent
false ; l’exemple active la [préservation des éditions](doc:syncthing#active-checkout-edits).
`conflict_copies` vaut également false par défaut ; l’exemple active les
[copies pour vecteurs concurrents](doc:syncthing#protocol-conflicts).
`resume_downloads` vaut false par défaut ; lire les [conditions du staging en clair](doc:syncthing#persistent-downloads) avant activation. Ces deux
politiques protègent des situations distinctes et se configurent séparément.

Ctrl+C ou SIGTERM arrête le processus. Redémarrer avec la même identité et la
même configuration reprend état natif/catalogue. La sortie JSON reste canonique ;
`ready` indique initialisation locale/écoute, pas synchronisation terminée.
Aide et diagnostics propres au CLI sont anglais/français ; `--lang` prime sur
`SYRNCK_LANG`, puis anglais. Les détails système/bibliothèque gardent leur langue.
Le CLI principal ouvre cette page par `syrnck --lang fr help syncthing --section standalone-service`.
Installation de service OS, orchestration de plusieurs pairs et sémantique
complète des dossiers/liens restent à terminer.


Pour un fournisseur du compte OS, utiliser `key_vault` et éventuellement `signing_key_vault` à la place des fichiers protégés. Voir les [paramètres de service des coffres système](doc:system-vaults#services) pour les usages exacts, exclusions, prérequis de trousseau/confiance macOS et règles de sauvegarde/suppression. Ces paramètres ne déplacent pas le PEM d’appareil TLS ou le mot de passe de dossier Syncthing dans un coffre.

## Construire et distribuer l’exécutable {#build-standalone-service}

Activer `-DSYRNCK_BUILD_BEP_SERVICE=ON` dans CMake (par défaut avec les tests
Syncthing). Construire d’abord `syrnck_ffi`. Avec Go 1.26.2+, préparer une fois les
dépendances publiques figées :

```text
python scripts/build_bep_service.py --download --library BIBLIOTHEQUE_NATIVE --output EXECUTABLE_BEP
```

Le preset Windows utilise `build/Release/syrnck_ffi.dll` et
`build/Release/syrnck-bep.exe` ; Linux : `build/libsyrnck_ffi.so` et
`build/syrnck-bep` ; macOS : `build/libsyrnck_ffi.dylib` et `build/syrnck-bep`.
Construire ensuite normalement : CMake appelle le même outil hors ligne. Unix
nécessite cgo et la bibliothèque native correspondante. Utiliser un build sans
sanitizer pour cet exécutable, séparé des contrôles ASan du cœur.

`cmake --install build --config Release --prefix dist` installe exécutable,
bibliothèque correspondante et manuel local. Les archives sources inchangées
des modules Go réellement utilisés, licences/notices, licence du runtime Go et
manifest d’empreintes sont conservés dans `share/licenses/Syrnck/bep`. Les garder
avec la distribution ; MIT du projet ne remplace pas les licences tierces.

Les tests Windows utilisent de vrais processus autonomes et le daemon amont figé,
avec Stores clairs/chiffrés, éditions dans les deux sens et renommage distant hors
ligne après redémarrage. Des arrêts brutaux distincts vérifient la reconstruction
des anciennes/nouvelles versions. Exécution et déplacement de distribution
Linux/macOS dépendent encore de leur CI ; la configuration ne les prouve pas.

Le service autonome accepte aussi les [clés de signature natives protégées](doc:signing-keys#services), avec `signing_key_file` et `signing_key_password_env`. L’authentification précède workers/écoute et utilise un propriétaire natif dédié entre sessions courtes.

## Appareils non fiables chiffrés {#untrusted}

Le client Syrnck de confiance peut échanger des fichiers chiffrés avec un véritable daemon Syncthing **Receive Encrypted**. Il utilise le codec amont figé et sa négociation par preuve de mot de passe, sans modifier Syncthing ni réimplémenter son format cryptographique. Ce mode est distinct des [snapshots de révisions Syrnck chiffrés](doc:sealed-repositories) : il transporte métadonnées/blocs de fichiers Syncthing, pas le graphe de commits natif. Le [récepteur opaque](doc:syncthing#opaque-receiver) distinct fournit le rôle receive-encrypted dans Syrnck.

Copier l’exemple installé `share/Syrnck/examples/bep-untrusted-service.json`, ou ajouter ces réglages à une configuration autonome existante :

```json
{
  "untrusted_peer": true,
  "folder_password_env": "SYRNCK_BEP_FOLDER_PASSWORD"
}
```

Fournir cette variable via un lanceur de confiance avec le même mot de passe de dossier non vide sur chaque client de confiance. Configurer le dossier du pair en `receiveencrypted` et utiliser le même identifiant de dossier. Le CLI lit une seule fois 1 à 4096 octets UTF-8 avant capture/écoute, puis retire la variable de son propre environnement, en cas de succès ou d’erreur. Il ne revient jamais au clair si le mot de passe configuré manque. Le nom du fournisseur doit différer de ceux des clés de stockage/signature et autres clés. Aucun mot de passe dans le JSON ou les arguments. Le processus lanceur reste inchangé. Ce secret est distinct d’une clé de Store, graine de signature, certificat d’appareil ou clé maîtresse de snapshots ; l’API native des clés chiffrées ne l’ouvre pas.

Les appelants du module définissent `FolderSessionOptions{Untrusted: true, EncryptionPassword: password}` pour la session ou son service. Laisser ces deux champs vides pour un pair de confiance ordinaire. `FolderService` réutilise son codec et la dérivation de clé après reconnexion, y compris après un échec temporaire de négociation initiale amont. L’appelant direct de `FolderSession` gère sa boucle de reconnexion. L’implémentation Go amont conserve des chaînes de mots de passe et des caches de clés dérivées ; elle n’offre pas les mécanismes d’effacement explicite des propriétaires natifs. Protéger le processus de confiance et choisir un mot de passe fort indépendant. Une preuve publique correspondante est exigée avant autorisation des index ou requêtes ; preuve absente, fausse ou contradictoire entraîne un refus. Une configuration initiale vide d’un nouveau dossier amont chiffré est acceptée pendant l’apprentissage de la preuve, sans autoriser les données. L’autorisation attend l’installation des clés du codec local.

Le client conserve une politique canonique de chiffrement pair/dossier et le hash de sa preuve de mot de passe dans un bloc immuable référencé par `ffi/bep/…/workspace/encryption/PEER_HASH`. Le démarrage lie le mode et le mot de passe avant ouverture de connexion, même si le pair est hors ligne. Les reconnexions ne réécrivent pas ce contrôle. Une première transition explicite du clair vers le chiffré est autorisée ; ensuite, retirer le chiffrement ou changer le mot de passe est refusé avant connexion. Garder ce point de contrôle dans les sauvegardes. Corriger une erreur de configuration par migration/reprovisionnement explicite ; supprimer un catalogue/contrôle pour le contourner n’est pas une rotation automatique. Rotation/révocation distribuée et protection contre une ancienne application qui ignore cette politique ne sont pas fournies.


Pour changer de mot de passe ou corriger son approvisionnement, arrêter les services concernés, conserver une sauvegarde vérifiée du Store, puis configurer un nouvel identifiant de dossier partagé et un mot de passe indépendant identique sur les clients de confiance et un dossier receive-encrypted neuf. Le Store/historique natif peut être gardé ; le nouvel identifiant crée un contexte causal/chiffré séparé et renvoie ses données courantes. Vérifier la restauration avant retrait explicite de l’ancien dossier distant. Cette procédure ne révoque pas les anciennes copies de mot de passe/données. Activer le chiffrement ne cache pas les données déjà envoyées à un ancien pair de confiance.

Les index chiffrés sont authentifiés/déchiffrés avant entrée dans le catalogue causal ; métadonnées altérées ou en clair ferment la connexion. Le codec amont déchiffre/authentifie les blocs, puis les descripteurs SHA-256 originaux sont vérifiés avant ingestion native. Les derniers blocs courts et blocs vides utilisent le padding Syncthing, tandis que le moteur ne lit que la plage originale annoncée exacte. Historique natif, application contrôlée, copies de conflit et reprise restent actifs. Staging de reprise et fichiers de travail demeurent en clair sur le client de confiance, avec leurs protections existantes.

Le pair non fiable voit identifiant/étiquette de dossier, tailles approximatives, horaires, historique chiffré et égalité au même nom/décalage. Chemins, métadonnées et contenus restent chiffrés. Syncthing dérive les clés de fichier du nom et chiffre les preuves de blocs avec leur décalage : changer de nom ou déplacer les octets peut exiger un nouvel envoi. Un bloc inchangé au même décalage peut être réutilisé. Consulter le [protocole amont](https://docs.syncthing.net/specs/untrusted.html) et le [guide utilisateur](https://docs.syncthing.net/users/untrusted.html) pour le modèle de menace et le statut actuel amont. L’authentification ne prouve pas qu’un pair qui retient les données est à jour ; conserver l’état causal de confiance. Confiance initiale/comparaison entre appareils et rétention distribuée restent distinctes.

La qualification Windows utilise le daemon figé non modifié : source de confiance, récepteur indépendant, delta d’un bloc au même décalage, reconnexions, puis appareil de confiance neuf restauré sans les deux premiers clients connectés. Elle vérifie aussi fichiers vides/derniers blocs courts, noms/données opaques sur disque, redémarrage réel de l’exécutable autonome, désaccord de preuve, refus persistant du retour en clair et métadonnées en clair/altérées sur TLS épinglé. Le fichier synthétique de 384 Kio change un bloc de 128 Kio ; l’envoi protocolaire mesuré est d’environ 129 Kio. Ce n’est pas une mesure de débit/RAM à l’échelle Unreal. L’exécution Linux/macOS courante dépend encore de la CI.


## Récepteur chiffré opaque {#opaque-receiver}

`EncryptedReceiver` conserve directement les métadonnées et blocs chiffrés amont dans le stockage natif Syrnck. Il n’a ni mot de passe de dossier, ni scanner, ni dossier de travail. Les pairs de confiance possèdent ce mot de passe ; le récepteur lie leur preuve publique au catalogue durable avant d’accepter les données. L’appartenance reste définie par les certificats épinglés. Preuve différente et rôle clair/mixte sont refusés. Le premier pair explicitement associé fournit cette preuve ; ce n’est pas un échange initial de confiance indépendant ni une rotation distribuée.

Utiliser `NewEncryptedReceiver(ctx, EncryptedReceiverOptions{Catalog: catalog, Storage: storage})` avec un Store initialisé. Un fournisseur `OpenStore` peut ouvrir des Stores courts avec une clé au repos distincte. `OpenEncryptedReceiverSession` authentifie un transport associé ; `OpenEncryptedReceiverService` ajoute reconnexion, attente progressive et arrêt joint. Les sessions renvoient les mêmes rapports/erreurs bornés de `FolderSession`. Plusieurs services de pairs explicites peuvent partager un récepteur. Fermer toutes les sessions/services avant celui-ci. Aucun Store natif ne reste ouvert pendant l’attente des blocs réseau.

L’exécutable autonome accepte un seul pair configuré dans ce rôle. Copier l’exemple installé `share/Syrnck/examples/bep-encrypted-receiver.json`, puis remplacer identité, chemins et identifiant du pair. Par exemple :

```json
{
  "version": 1,
  "receive_encrypted": true,
  "storage": "opaque-store",
  "folder": "project",
  "certificate": "device-cert.pem",
  "private_key": "device-key.pem",
  "peer": "REPLACE_WITH_EXPLICIT_PAIRED_DEVICE_ID",
  "mode": "listen",
  "address": "127.0.0.1:22001"
}
```

Initialiser `storage` avec le CLI natif ordinaire avant lancement. Les chemins sont relatifs au fichier JSON. Configurer chez le pair de confiance le mot de passe du dossier pour ce récepteur ; les clients Syrnck de confiance utilisent `untrusted_peer: true`. Le récepteur doit omettre `directory`, `workspace`, `untrusted_peer`, `folder_password_env`, les champs d’auteur/signature et les politiques de fichiers de travail. Il ne déchiffre rien et ne crée pas de révisions en clair signées. Les fournisseurs de stockage `key_env`/`key_file` facultatifs protègent indépendamment ses objets physiques natifs. L’événement `ready` contient `receive_encrypted: true` ; ce n’est pas un reçu de synchronisation.

Un catalogue distinct `ffi/bep-encrypted/…` retient preuve publique, générations/index des pairs, enveloppes chiffrées originales et liaisons complètes aux manifestes natifs de chaque bloc. Les reçus de blocs terminés sous `ffi/bep-receipts/…` protègent les téléchargements interrompus après redémarrage et GC. La reprise ne demande que les blocs manquants liés à leur preuve. Après rétention du fichier complet par l’historique parent du catalogue, les racines exactes des reçus sont retirées par comparaison atomique gardée par le catalogue. Les reçus incomplets dépassés restent conservés ; leur nettoyage automatique n’est pas implémenté. Garder ces références/historiques dans les sauvegardes, sans les supprimer au seul motif qu’un nom de travail a disparu.

Le récepteur compare des preuves opaques de blocs de 48 octets ; il ne peut pas les vérifier comme des hashes du contenu en clair. Les hashes natifs vérifient l’intégrité du chiffré stocké ; les clients de confiance authentifient/déchiffrent puis vérifient les données originales. Le chiffré est réutilisé si nom/décalage/preuve/taille restent identiques. Les blocs BEP chiffrés maximaux de 16 Mio comportent 40 octets supplémentaires et utilisent deux manifestes natifs pour préserver la borne native de 16 Mio des opérations en mémoire. Seuls les fichiers complets sont annoncés. Les métadonnées chiffrées des dossiers/liens sont conservées et retransmises sans matérialisation ; l’application au workspace de confiance conserve ses limites documentées. Aucun export d’arbre de fichiers/trailers chiffrés Syncthing standard n’est produit.

Les compteurs opaques cachent les vecteurs causaux privés. Toutes les versions de pairs et les anciennes racines du catalogue restent retenues ; la version annoncée utilise le plus grand compteur opaque puis l’identifiant d’appareil source pour départager les égalités de manière stable. Cela ne résout pas les conflits en clair. Les clients déchiffrent le vecteur réel et appliquent leur politique existante. Il n’y a ni preuve serveur qu’un chiffré fourni par un pair autorisé malveillant est déchiffrable, ni garantie de fraîcheur contre la rétention de données, rétention automatique, rotation de mot de passe/clé ou orchestration d’écoute CLI multipair.

Les tests Windows couvrent l’envoi depuis un vrai daemon Syncthing de confiance vers ce récepteur, puis la restauration par un client Syrnck neuf après arrêt de la connexion source et GC natif. D’autres tests couvrent fichiers courts/vides, delta au même décalage, restauration historique, blocs maximaux bornés, reprise de blocs interrompus en Stores natifs clairs/chiffrés, services de pairs simultanés avec vraie perte de connexion, mauvaises preuves/rôles, possession/bornes de la file et arrêt brutal/redémarrage d’un vrai processus autonome. Cela ne prouve ni durabilité après coupure électrique, ni ARM64, ni performances à l’échelle Unreal ; Linux/macOS restent soumis aux résultats actuels de CI.
