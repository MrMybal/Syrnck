---
id: go-binding
title: Binding Go
description: Guide : Binding Go.
section: API
order: 10
parent: integrations
sourceRevision: 2
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

Le module optionnel [Syncthing](doc:syncthing) adapte maintenant les manifestes aux blocs fixes SHA-256 de BEP et aux requêtes de la bibliothèque Syncthing v2.1.7. Il prend en charge la reconstruction différentielle et un staging reprenable, avec le scanner et les connexions BEP TLS 1.3. Les objets et le CDC Syrnck gardent leur format propre. Le daemon résident, la découverte, les conflits et la synchronisation complète entre exécutables Syncthing restent à réaliser.

## Registered workspaces {#registered-workspaces}

Ouvrir avec `WorkspaceOptions{Name: "edit"}` puis sélectionner
`store.UseWorkspace("edit")` avant chaque statut. Un nom vide utilise le principal.
Un contexte différent échoue ; un nom invalide garde la sélection précédente.
Voir [les contextes liés](doc:workspace-abi#registered-contexts).
