---
id: syncthing
title: Adaptateur de blocs Syncthing
description: Guide : Adaptateur de blocs Syncthing.
section: API
order: 660
parent: integrations
sourceRevision: 1
---

# Adaptateur Syncthing à blocs fixes {#syncthing-fixed-block-adapter}

Ce module Go optionnel relie le C++ au modèle de blocs BEP via la bibliothèque Syncthing non modifiée **v2.1.7**, commit `d6d0181f7d46`, pseudo-version figée dans go.mod/go.sum. Le binding cœur reste séparé, sans dépendances Go externes. Le module demande Go 1.26.2+.

Il gère les données des fichiers ordinaires. Daemon résident, exécutable Syncthing interchangeable, base de données, découverte, appairage, vecteurs de versions, suppressions, renommages et politique de copies conflictuelles restent à la charge de l’application intégratrice.

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


## Reprise et annulation {#resume-and-cancellation}

Sans `ResumeDirectory`, Fetch utilise un fichier temporaire privé retiré au retour. Avec cette option, le dossier par contenu appartient à l’appelant : `state.json`, `data`. Le descripteur lie folder/nom/taille/taille de bloc et hash de liste complète. Autre index, fichier inconnu, descripteur incomplet ou lien est refusé. Utiliser un dossier privé et sérialiser ses gestionnaires.

Chaque plage de staging est rehashée au retry. Seules les plages complètes conformes sont réutilisées ; partielles/corrompues redemandées. Les trous remplis de zéro peuvent satisfaire un bloc zéro sans requête. Flush tous les 16 blocs et avant ingestion. Annulation préserve le staging fourni par l’appelant, qui reste **en clair même avec Store chiffré** ; protéger l’emplacement. Le retrait des dossiers terminés est explicite.

Le contexte est vérifié entre appels natifs/étapes réseau/blocs ; ingestion/vérification synchrone actuelle ne s’annule pas au milieu. Une annulation tardive ou changement détecté peut laisser un manifeste complet retenu, dont l’ID accompagne l’erreur. Traiter l’erreur avant publication. Aucun fichier de travail partiel installé. Un kill peut laisser du privé ; durabilité après coupure et retrait automatique ne sont pas établis.
