---
id: syncthing
title: Adaptateur de blocs Syncthing
description: Guide : Adaptateur de blocs Syncthing.
section: API
order: 660
parent: integrations
sourceRevision: 2
---

# Adaptateur Syncthing à blocs fixes {#syncthing-fixed-block-adapter}

Ce module Go optionnel relie le C++ au modèle de blocs BEP via la bibliothèque Syncthing non modifiée **v2.1.7**, commit `d6d0181f7d46`, pseudo-version figée dans go.mod/go.sum. Le binding cœur reste séparé, sans dépendances Go externes. Le module demande Go 1.26.2+.

Il gère les fichiers ordinaires et les connexions authentifiées entre appareils explicitement associés. Daemon résident, remplacement de l’exécutable Syncthing, base de données, découverte, interface d’appairage, réconciliation persistante des vecteurs de versions, suppressions/renommages du workspace et politique de conflits restent à la charge de l’application intégratrice.

## Bibliothèque native et dépendances {#build-and-test}

Compiler `syrnck_ffi` et télécharger une fois les dépendances Go figées avec `go mod download` depuis `bindings/syncthing`. L’adaptateur est optionnel, séparé du binding cœur. Linux/macOS utilisent cgo et le chemin du chargeur approprié ; ces plateformes et ARM64 restent à valider. Ne distribuer que les dépendances et notices requises pour l’intégration choisie.

## API {#apis}

`NewFile(ctx,store,manifest,folder,name,blockSize)` crée une vue immuable servable. Il vérifie le hash complet sans exporter puis lit des plages vérifiées pour construire les `protocol.FileInfo` SHA-256. CDC/blocs fixes et IDs BLAKE3 du moteur restent intacts. Taille 0 : politique Syncthing ; sinon puissance de 2 de 128 Kio à 16 Mio. Index limité 65536 blocs. Fichier vide : bloc SHA-256 vide unique de référence.

`Info()` copie l’index indépendamment ; l’application y pose vecteurs de version, séquences et timestamps avant annonce. Noms relatifs NFC UTF-8 valides ; séparateurs Windows convertis en `/` protocolaire.

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
planification, état persistant des dossiers/index, vecteurs de versions,
tombstones et résolution des conflits. Le chiffrement du transport protège et
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
fichiers ordinaires ; le service autonome et persistant de dossiers reste à faire.
Sans binaire explicite, ce test annonce qu’il est ignoré ; les tests ordinaires
de l’adaptateur continuent de s’exécuter.

## Reprise et annulation {#resume-and-cancellation}

Sans `ResumeDirectory`, Fetch utilise un fichier temporaire privé retiré au retour. Avec cette option, le dossier par contenu appartient à l’appelant : `state.json`, `data`. Le descripteur lie folder/nom/taille/taille de bloc et hash de liste complète. Autre index, fichier inconnu, descripteur incomplet ou lien est refusé. Utiliser un dossier privé et sérialiser ses gestionnaires.

Chaque plage de staging est rehashée au retry. Seules les plages complètes conformes sont réutilisées ; partielles/corrompues redemandées. Les trous remplis de zéro peuvent satisfaire un bloc zéro sans requête. Flush tous les 16 blocs et avant ingestion. Annulation préserve le staging fourni par l’appelant, qui reste **en clair même avec Store chiffré** ; protéger l’emplacement. Le retrait des dossiers terminés est explicite.

Le contexte est vérifié entre appels natifs/étapes réseau/blocs ; ingestion/vérification synchrone actuelle ne s’annule pas au milieu. Une annulation tardive ou changement détecté peut laisser un manifeste complet retenu, dont l’ID accompagne l’erreur. Traiter l’erreur avant publication. Aucun fichier de travail partiel installé. Un kill peut laisser du privé ; durabilité après coupure et retrait automatique ne sont pas établis.
