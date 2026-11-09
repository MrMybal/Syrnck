---
id: go-binding
title: Binding Go
description: Guide : Binding Go.
section: API
order: 10
parent: integrations
sourceRevision: 13
---

# Binding Go du moteur C++ {#binding-go-du-moteur-c}

Le module local `syrnck.local/bindings/go` expose les stores, le découpage CDC/fixe, la lecture/importation des objets, leurs dépendances, les pins et la reconstruction vérifiée. Il n'ajoute aucune dépendance Go externe. Dans un projet consommateur, utiliser temporairement un `replace` vers ce dossier, jusqu'à la publication du module sous une adresse définitive.

Le moteur reste entièrement en C++20 ; ce dossier contient seulement son adaptateur Go. Windows amd64/arm64 utilise le chargeur DLL du système, sans cgo ni compilateur C supplémentaire. Linux/macOS utilise cgo et lie `syrnck_ffi`. Seul Windows amd64 a été exécuté localement ; Linux/macOS sont prévus dans la CI. ARM64 et les autres plateformes restent à valider.

## Charger la bibliothèque native {#tester}

Compiler la bibliothèque C++ correspondante. Sous Windows, définir `SYRNCK_LIBRARY` au chemin absolu de la DLL de confiance avant le premier `Open` ; elle reste chargée pour la durée du processus. Unix utilise cgo : configurer `CGO_LDFLAGS=-L<dossier>` et le chemin du chargeur ou un rpath de distribution. Go 1.22+ est requis.

## Exemple {#exemple}

```go
source, err := syrnck.Open("source-store", syrnck.Options{})
if err != nil { return err }
defer source.Close()
destination, err := syrnck.Open("destination-store", syrnck.Options{Create: true})
if err != nil { return err }
defer destination.Close()

id, err := source.Ingest("Map.uasset", syrnck.Chunking{})
if err != nil { return err }
stats, err := syrnck.CopyMissing(ctx, source, destination,
    syrnck.ObjectKey{Kind: "manifest", ID: id}, "sync/map-v1")
if err != nil { return err }
fmt.Printf("%d octets copiés\n", stats.Bytes)
return destination.Restore(id, "Map-restored.uasset")
```

`Create` exige un dossier neuf ou vide ; il ne signifie pas « ouvrir ou créer ». `Options.Key` contient zéro ou 32 octets pour le chiffrement au repos. L'appel ne conserve pas le tableau Go. L'application reste responsable du cycle de vie de sa clé et de l'effacement de ses propres copies.

## Préparer et publier un état composé {#catalog-transactions}

`PrepareBytes(data, chunking)` ingère au plus 16 Mio depuis la mémoire sans
fichier temporaire en clair. `PrepareFile(path, chunking)` lit un flux sans cette
limite d’entrée. Les deux rendent des manifests **sans référence**, contrairement
à l’ancien `Ingest`. `Put(kind, data)` crée un objet typé sans référence : octets
de blocs exacts ; C++ valide et encode canoniquement les JSON de manifest/révision
et calcule leur ID BLAKE3 natif. Champs inconnus, clés dupliquées même échappées,
schéma invalide, profondeur et tailles excessives sont refusés. Aucun encodeur
canonique ni algorithme de hash d’objet natif n’est à réimplémenter en Go.

Garder le Store ouvert pendant préparation et publication. Après fermeture,
le GC peut supprimer les objets sans référence. Retenir les dépendances dans une
révision native complète ou des racines explicites : un bloc opaque contenant
des IDs ne retient pas automatiquement ces objets. Les fichiers et parents d’une
révision native tracent leurs dépendances complètes pour GC et sauvegardes.

`Refs()` rend une `map[string]ObjectKey` indépendante. `CompareRefs(updates)`
protège et remplace/supprime atomiquement au plus 65536 références et 16 Mio de
JSON. Chaque fermeture et hash de fichier complet sont vérifiés avant l’unique
publication de la table. Les noms en double sont des erreurs. `Expected` nul
exige l’absence ; `Value` nul supprime. Un groupe nil/vide ne change rien.
Le booléen n’a de sens que si `err == nil` :

```go
current, err := store.RefsContext(ctx)
if err != nil { return err }
var expected *syrnck.ObjectKey
if old, ok := current["ffi/catalog/main"]; ok { expected = &old }

// JSON applicatif d’une révision native complète référençant les manifests
// préparés et les éventuelles révisions parentes à conserver.
candidate, err := store.PutContext(ctx, "revision", revisionJSON)
if err != nil { return err }
applied, err := store.CompareRefsContext(ctx, []syrnck.RefUpdate{
    {Name: "ffi/catalog/main", Expected: expected, Value: &candidate},
})
if err != nil { return err } // Relire et établir le résultat avant un nouvel essai.
if !applied { return errRefreshAndReplan }
```

Une condition périmée ne change aucun membre et rend false sans erreur.
Relire/replanifier au lieu de changer la valeur attendue d’une ancienne opération.
Une erreur IO peut survenir après publication atomique : sur erreur, le booléen
ne prouve pas l’absence de publication. Rouvrir/relire la table et comparer le
groupe exact proposé avant de réessayer. La table est entièrement ancienne ou
nouvelle, sans mélange produit par cette transaction. Un parcours de haut niveau
doit conserver son intention s’il doit réconcilier un résultat inconnu après crash.

Les cinq opérations possèdent une variante `Context`. Préparation et CAS propagent
l’annulation native pendant ingestion/vérification et avant publication ; un
succès terminé l’emporte sur une demande tardive. `PutContext` et `RefsContext`
contrôlent l’annulation avant leur appel natif borné ; encodage/compression/lecture
déjà engagés peuvent finir. Une préparation échouée peut laisser des objets
complets sans référence, sans publication. Les écrivains externes concurrents
restent à gérer par l’application. Ces primitives permettent un état protocolaire
persistant ; elles n’implémentent pas la réconciliation Syncthing ni le checkout.

## Concurrence et conservation {#concurrence-et-conservation}

Ne pas copier un `Store`. Les méthodes et `Close` partagent un mutex. Chaque séquence native reste sur le même thread OS pour récupérer correctement les erreurs C locales au thread. Aucun pointeur Go n'est conservé par C++. Fermer explicitement chaque store pour libérer son verrou de processus.

`Ingest` conserve automatiquement le fichier avec un pin `ffi/<manifest>`. `Import` vérifie l'identifiant et le format, mais ne publie rien. `Pin` vérifie toute la fermeture et les hashes complets avant de remplacer la référence ; conserver plusieurs noms de pins pour garder plusieurs versions. `Unpin` rend éventuellement des objets éligibles à un futur GC.

`CopyMissing` est un banc d'intégration local entre deux handles : il verrouille les deux stores dans un ordre stable, compare les présences vérifiées, copie un objet à la fois, puis publie le pin. Une reprise réutilise les objets déjà écrits, y compris après fermeture/réouverture. L'annulation intervient entre objets. Un transfert interrompu laisse des objets non publiés qui doivent rester protégés contre le GC jusqu'à sa reprise. Il n'y a pas de GC concurrent possible à travers ce binding ; un GC lancé après fermeture peut supprimer ces objets non épinglés.

Le corps d'un objet est limité à 16 Mio. La liste complète des dépendances est chargée en mémoire, limitée à 128 Mio côté Go. Les lectures par buffers font actuellement une requête de taille puis une lecture, donc deux vérifications natives. La pagination, les leases persistants, les buffers réutilisables et le coût du passage Go/C++ restent des travaux de performance.

## Plages et comparaison de fichiers {#plages-et-comparaison-de-fichiers}

`ReadRange(id, offset, length)` retourne les octets d'une plage exacte, dans la
limite de 16 Mio par appel. Les blocs sélectionnés sont vérifiés ; une plage
couvrant tout le fichier vérifie aussi son hash complet. `RestoreRange` exporte
en flux vers un fichier neuf, sans cette limite de longueur, avec publication
exclusive. Une plage hors limites renvoie une erreur sans troncature.

`CompareFiles(source, target)` retourne un `FileComparison` typé : compteurs
d'octets réutilisables/nouveaux, blocs cibles avec offset source éventuel et liste
dédupliquée des blocs absents du manifeste source. Ce plan repose uniquement sur
les métadonnées. Vérifier les blocs copiés et le fichier terminé avant publication.
Il ne prouve pas qu'un fichier de travail contient toujours les octets attendus.

Ces méthodes partagent le mutex et le thread OS des autres appels. Elles ne
changent aucun pin et ne sont pas annulables pendant un appel natif. Le JSON de
comparaison est plafonné à 128 Mio puis décodé en structures Go. La bibliothèque
et les headers doivent provenir de cette version ; le numéro ABI 1 est conservé
avec des symboles supplémentaires. Voir les contrats des [plages](doc:file-ranges)
et [comparaisons](doc:file-comparison).

## Observation du workspace {#workspace-observation}

`OpenWorkspace(project, storePath, WorkspaceOptions{})` arme le watcher et garde l’inventaire complet sans Store ouvert. Utiliser `Status(store, WorkspaceSelection{})`, ou `Combined: true` pour index+travail. Fermer le Store de l’appelant après chaque requête, puis `Wait(ctx, timeout)` pour des hints natifs/polling typés. Les lots restent retenus jusqu’à status réussi, même après plusieurs waits ou annulation. `Invalidate` demande un audit complet ; les planifier indépendamment des notifications.

Les options nulles utilisent Windows/Linux natif, fallback polling, 4096 hints et 65536 enregistrements. Appels et `Close` sont sérialisés ; fermer explicitement. `Status` retourne le JSON canonique en `json.RawMessage`. `OpenWorkspaceContext`/`StatusContext` transmettent le contexte aux enregistrements récursifs et au hash ; `Wait` interrompt directement l’attente native. L’annulation peut aussi quitter l’attente du mutex workspace/Store. La source native est privée à chaque appel, libérée après arrêt/fin de son callback. Un résultat natif terminé gagne sur une annulation tardive ; IO système/encodage JSON peuvent finir avant le prochain contrôle. Les callbacks C de progression ne sont pas encore exposés en Go. Voir [ownership et limites](doc:workspace-abi).

## Fermeture sous Windows {#windows-test-cleanup}

Fermer explicitement les Stores et sessions avant de retirer leurs répertoires. La DLL de confiance reste chargée pour la durée du processus.

## Syncthing {#syncthing}

Le module optionnel [Syncthing](doc:syncthing) adapte les manifests aux blocs fixes SHA-256 BEP via la bibliothèque Syncthing v2.1.7 figée. Connexions d’appareils authentifiées et transferts différentiels sont vérifiés contre son daemon réel. Objets et CDC Syrnck gardent leur format. Le catalogue conserve index, vecteurs, tombstones et données vérifiées dans le moteur ; le téléchargement séparé ne garde pas le Store pendant l’attente réseau. Le [contrôleur continu de fichiers](doc:syncthing#folder-controller) applique les versions vérifiées aux workspaces natifs avec reprise à propriétaire. Sémantique complète des dossiers/liens, copies pour versions protocolaires concurrentes, découverte, interface d’appairage et installation de service OS restent à terminer.

## Workspaces enregistrés {#registered-workspaces}

Ouvrir avec `WorkspaceOptions{Name: "edit"}` puis sélectionner
`store.UseWorkspace("edit")` avant chaque statut. Un nom vide utilise le principal.
Un contexte différent échoue ; un nom invalide garde la sélection précédente.
Voir [les contextes liés](doc:workspace-abi#registered-contexts).

## Capture et reprise natives {#native-workspace-actions}

`workspace.NativeState(store)` lit contexte natif sélectionné, référence,
révision, matérialisation et checkout en cours. `state.Expected()` fige les
identités cible/révision/politique exactes. L’utiliser pour
`Capture(store, expected, message)`, `PlanCheckout(store, expected, candidate)`
et `CheckoutOwned(store, expected, candidate, owner)`. Capture conserve le travail
local ; checkout exige un workspace propre et garde un journal récupérable.
`RecoverOwned(store, target, candidate, owner)` reprend uniquement cette opération
en cours exacte. Les cinq méthodes ont des variantes Context et vérifient la
liaison exacte Store/contexte/dossier. Une erreur de mutation exige une relecture :
la publication peut déjà avoir eu lieu. Voir le [contrat des actions natives](doc:workspace-abi#native-workspace-actions).

`CheckoutStatusContext(ctx, store, transaction)` lit un journal ou reçu terminé
exact par le décodeur du cœur, avec liaison Store/contexte/dossier vérifiée. Un
reçu committed prouve une application passée, même après une édition locale ; il
ne prétend pas que le workspace est encore propre.

`RecoverOwnedPreserving[Context]` archive explicitement les éditions de fichiers
ordinaires et crée des copies exclusives avant reprise du journal natif exact.
Respecter le [contrat des éditions actives](doc:workspace-abi#active-checkout-edits) ;
`RecoverOwned` reste strict et toute erreur de mutation exige une relecture.

## Rapport des E/S de capture {#incremental-capture}

`CaptureReportContext` retourne révision et compteurs sources de lecture et
réutilisation. `CaptureContext` conserve son résultat limité à l’ID. Les
manifestes de session peuvent survivre à la réouverture du Store, avec inventaire
et vérification complète. Voir les [limites](doc:performance#incremental-capture).

## Auteurs natifs et nouvelles révisions {#native-authorship}

`UseNativeIdentity(identity, seed)` choisit auteur/committer pour un handle Store.
Sans graine, les métadonnées v2 sont non signées ; une graine de 32 octets active
Ed25519 natif. `NativeIdentity` contient nom, email et décalage de fuseau en minutes
vers l’est de UTC. UTF-8 invalide, contrôles, longueurs excessives et décalages
hors -1439..1439 sont refusés. Un remplacement échoué garde l’identité précédente.

`ConfigureNativeIdentity(identity, NativeIdentityOptions{Seed: seed,
RequireSignature: true})` impose une signature. Sans graine disponible, lecture
et reprise restent possibles mais captures/préparations neuves échouent, sans
repli non signé. `ClearNativeIdentity()` retire explicitement identité/politique
du handle. `NativeIdentityMetadata()` retourne identité, identifiants publics et
le drapeau actif `signature_required`, jamais la graine.

`PrepareNativeRevisionContext(ctx, draft)` accepte un JSON borné de brouillon v1
neuf. C++ le canonicalise, vérifie parents/fichiers complets, puis applique
identité/signature du handle. Un objet v2 est refusé sans remplacer son auteur.
La révision rendue sans référence doit être publiée par refs protégées avant
libération du Store au GC. Aucune ref ni fichier de travail n’est modifié.
L’annulation couvre la vérification ; une erreur IO tardive peut laisser un objet
non référencé ou le drapeau de lecteur requis. `PutContext` brut conserve son
contrat explicite de métadonnées de bas niveau.

L’ABI C expose `store_use_native_identity`, `store_clear_native_identity`,
`store_native_identity_json` et `prepare_native_revision_ex` dans le header
livré. Aucun pointeur secret n’est retenu. La clé native est effacée au
remplacement/clear/Close ; l’appelant contrôle, sérialise et efface ses buffers
de graine. Ces API n’ajoutent aucune clé privée aux configurations/sauvegardes.

Un fournisseur résident configure chaque nouveau Store avant de le rendre.
Arrêter ses consommateurs avant de retirer le fournisseur. Le contrôleur
Syncthing utilise ce contexte pour captures locales et révisions natives
entrantes synthétisées ; sa [configuration autonome](doc:syncthing#native-authorship)
accepte auteur explicite et variable de graine distincte. Une signature locale
n’est ni identité d’appareil BEP ni permission serveur. La publication serveur
garde sa politique de clés/identités autorisées. Le coffre OS reste à développer.

## Propriétaires natifs de clés protégées {#protected-keys}

`ProtectKey(key, password, PasswordKdfOptions{})` renvoie une enveloppe authentifiée de 104 octets ; une clé vide génère une nouvelle clé de stockage aléatoire. Les buffers de clé/mot de passe appartiennent à l'appelant. `ChangeKeyPassword` conserve la clé de stockage et les coûts non précisés ; les anciennes enveloppes ne sont pas révoquées. Les champs nuls choisissent 64 Mio/trois passes pour la protection et conservent les coûts existants lors du changement de mot de passe.

`OpenProtectedKey(envelope, password)` authentifie une fois dans un propriétaire natif, sans export en clair. Réutiliser `owner.Open(path, create)` pour les services résidents et fermer explicitement le propriétaire. Appels et Close sont sérialisés ; ne pas copier le propriétaire. Chaque Store garde sa propre clé : fermer le propriétaire ne révoque ni ne ferme les Stores ouverts.

```go
func openProtectedStore(path string, envelope, password []byte) (*syrnck.Store, error) {
    owner, err := syrnck.OpenProtectedKey(envelope, password)
    if err != nil { return nil, err }
    defer owner.Close()
    return owner.Open(path, false)
}
```

`OpenProtected(path, envelope, password, create)` fournit ce raccourci ponctuel. Pour les callbacks de service, garder un propriétaire vivant jusqu'à la jonction de tous les workers, et ouvrir des sessions Store courtes depuis celui-ci. La dérivation est synchrone et bornée ; les mots de passe contiennent de 1 à 4096 octets exacts, NUL compris pour le SDK. Effacer les buffers de l'appelant lorsqu'ils ne sont plus nécessaires. Un échec d'authentification ne crée pas de dossier Store.

Les [fichiers de clés CLI](doc:key-files) utilisent la même enveloppe. Associer ce binding à la bibliothèque native correspondante ; l'ABI 1 reçoit les nouveaux points d'entrée de clés.

## Signataires protégés {#protected-signers}

Les [enveloppes de signature dédiées](doc:signing-keys) utilisent
`ProtectSigningKey`, `ChangeSigningKeyPassword` et `OpenProtectedSigner`.
`SigningKey` Go ne doit pas être copié ; métadonnées publiques et Close explicite.
`NativeIdentityOptions.Signer` avec `RequireSignature: true` configure un état
indépendant pour chaque session courte sans refaire Argon2id. Seed et Signer
s’excluent. Fermer le propriétaire ne révoque ni Store configuré ni ancienne
enveloppe ; un échec préserve l’identité précédente.

## Propriétaires de dépôts chiffrés {#sealed-repositories}

`ProtectSealedKey`, `ChangeSealedKeyPassword` et `OpenProtectedSealedKey` utilisent l'[enveloppe maîtresse chiffrée distincte](doc:sealed-repositories#key-files) de 104 octets. `SealedKey` garde la clé déchiffrée dans la mémoire native sans export en clair. `PublicMetadata` rend seulement version/type/domaine public. Garder le propriétaire vivant jusqu'à la fin des opérations, puis fermer explicitement. Mots de passe/clés fournis restent à l'appelant ; effacer ces buffers lorsqu'ils ne servent plus. Une tranche de clé/graine vide sélectionne la génération pour les trois protecteurs : stockage, signature et chiffrement de bout en bout.

`SealSnapshot`, `InspectSnapshot` et `OpenSnapshot` acceptent contexte, racines typées, canal explicite et `SealedLimits`. Des limites nulles sélectionnent les défauts du cœur. La préparation vérifie la fermeture originale et retient un snapshot opaque ; l'inspection reste en lecture seule. L'ouverture importe les originaux vérifiés et retient atomiquement racines/point de contrôle sans changer HEAD natifs, staging ou fichiers. Un point existant refuse recul/divergence. Un point séparé permet une restauration historique explicite.

La fonction suivante reçoit des Stores source/transport/destination ouverts et distincts et une racine originale vérifiée. Le transport doit être hors des Stores en clair, dossiers conventionnels de projets et workspaces enregistrés. `previous` est un ID chiffré précédent exact, ou vide pour le premier snapshot.

```go
func restoreSealed(ctx context.Context, source, wire, destination *syrnck.Store,
    envelope, password []byte, root syrnck.ObjectKey, previous string,
) (syrnck.SealedSnapshotReport, error) {
    master, err := syrnck.OpenProtectedSealedKey(envelope, password)
    if err != nil { return syrnck.SealedSnapshotReport{}, err }
    defer master.Close()
    prepared, err := master.SealSnapshot(ctx, source, wire, "main",
        map[string]syrnck.ObjectKey{"heads/main": root}, previous, syrnck.SealedLimits{})
    if err != nil { return syrnck.SealedSnapshotReport{}, err }
    return master.OpenSnapshot(ctx, wire, destination, prepared.Snapshot, "main",
        "sealed/checkpoints/local-restore", syrnck.SealedLimits{})
}
```

Les rapports contiennent noms/IDs originaux privés et restent côté client. Transférer seulement `ObjectKey{Kind: "revision", ID: report.Snapshot}` et sa fermeture de transport ordinaire. Le helper `CopyMissing` existant permet de tester cette copie opaque localement sans clé maîtresse. Publication/réception HTTP reprenables de production utilisent actuellement le [workflow chiffré C++/CLI](doc:sealed-repositories#remote) ; ses wrappers CAS/sessions distants ne font pas partie de cette API Go. Agent résident et service de dossiers BEP attendent encore leur intégration chiffrée.

Go sérialise propriétaire et Stores participants dans un ordre de Stores cohérent, avec attentes de verrou annulables par contexte. Close concurrent attend la fin de l'usage natif. L'annulation demande la source native unique et joint cette demande avant libération ; une publication réussie gagne sur une demande d'arrêt ultérieure. IO/allocation de sortie peuvent encore échouer après publication : relire les références et prouver le résultat avant une relance. Ces API ne fournissent ni rotation/révocation distribuée, ni coffre OS, ni retrait automatique de l'historique chiffré.
