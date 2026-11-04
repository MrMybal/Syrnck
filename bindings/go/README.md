# Binding Go du moteur C++

Le module local `syrnck.local/bindings/go` expose les stores, le découpage CDC/fixe, la lecture/importation des objets, leurs dépendances, les pins et la reconstruction vérifiée. Il n'ajoute aucune dépendance Go externe. Dans un projet consommateur, utiliser temporairement un `replace` vers ce dossier, jusqu'à la publication du module sous une adresse définitive.

Le moteur reste entièrement en C++20 ; ce dossier contient seulement son adaptateur Go. Windows amd64/arm64 utilise le chargeur DLL du système, sans cgo ni compilateur C supplémentaire. Linux/macOS utilise cgo et lie `syrnck_ffi`. Seul Windows amd64 a été exécuté localement ; Linux/macOS sont prévus dans la CI. ARM64 et les autres plateformes restent à valider.

## Tester

Compiler d'abord le projet C++, puis :

```text
python scripts/test_go.py --library build/Release/syrnck_ffi.dll
```

Le script accepte `--go <exécutable>` et découvre aussi `.tools/go/bin/go.exe`. Sur Unix, passer `build/libsyrnck_ffi.so` ou `build/libsyrnck_ffi.dylib`. Il configure les chemins de liaison et de chargement pour ce processus. Les tests Go sont aussi ajoutés à CTest si Go est disponible à la configuration ; `SYRNCK_TEST_GO=OFF` permet de les désactiver.

Pour un programme Windows, définir `SYRNCK_LIBRARY` avec le chemin **absolu** de la DLL de confiance avant le premier appel à `Open`. Le binding charge et conserve cette DLL pour la durée du processus. Sur Unix, configurer `CGO_LDFLAGS=-L<dossier>` et le chemin du chargeur ou un rpath de distribution. Go 1.22+ est requis ; les tests Windows locaux utilisent Go 1.27.3.

## Exemple

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

## Concurrence et conservation

Ne pas copier un `Store`. Les méthodes et `Close` partagent un mutex. Chaque séquence native reste sur le même thread OS pour récupérer correctement les erreurs C locales au thread. Aucun pointeur Go n'est conservé par C++. Fermer explicitement chaque store pour libérer son verrou de processus.

`Ingest` conserve automatiquement le fichier avec un pin `ffi/<manifest>`. `Import` vérifie l'identifiant et le format, mais ne publie rien. `Pin` vérifie toute la fermeture et les hashes complets avant de remplacer la référence ; conserver plusieurs noms de pins pour garder plusieurs versions. `Unpin` rend éventuellement des objets éligibles à un futur GC.

`CopyMissing` est un banc d'intégration local entre deux handles : il verrouille les deux stores dans un ordre stable, compare les présences vérifiées, copie un objet à la fois, puis publie le pin. Une reprise réutilise les objets déjà écrits, y compris après fermeture/réouverture. L'annulation intervient entre objets. Un transfert interrompu laisse des objets non publiés qui doivent rester protégés contre le GC jusqu'à sa reprise. Il n'y a pas de GC concurrent possible à travers ce binding ; un GC lancé après fermeture peut supprimer ces objets non épinglés.

Le corps d'un objet est limité à 16 Mio. La liste complète des dépendances est chargée en mémoire, limitée à 128 Mio côté Go. Les lectures par buffers font actuellement une requête de taille puis une lecture, donc deux vérifications natives. La pagination, les leases persistants, les buffers réutilisables et le coût du passage Go/C++ restent des travaux de performance.

## Plages et comparaison de fichiers

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
avec des symboles supplémentaires. Voir les contrats anglais des [plages](../../docs/FILE_RANGES.md)
et [comparaisons](../../docs/FILE_COMPARISON.md).

## Windows test cleanup

The tests close native handles before removing their owned temporary directories.
The concurrency test additionally reopens the encrypted Store after a rejected
wrong-key open, verifying that failed construction releases its exclusive lock.
Windows cleanup waits at most two seconds for `ERROR_DIR_NOT_EMPTY`; other
errors retain `testing.TempDir`'s ordinary cleanup behavior. Persistent failures
still fail the test. Native calls and functional assertions are never retried.

## Syncthing

Ce binding prépare l'utilisation du moteur depuis Go. Il ne modifie pas Syncthing et n'implémente pas BEP : les hashes BLAKE3 avec domaine, les manifestes et le CDC Syrnck ne correspondent pas à son protocole. Même le mode fixe conserve le format Syrnck. Une future intégration devra adapter explicitement les index et échanges, ou négocier une extension entre pairs modifiés. Voir la [spécification BEP](https://docs.syncthing.net/specs/bep-v1.html) et les [règles cgo](https://pkg.go.dev/cmd/cgo#hdr-Passing_pointers).
