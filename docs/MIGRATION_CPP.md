# Migration vers C++20

La bibliothèque, le CLI, le serveur, l'agent, les filtres Git et l'implémentation de l'interface C sont désormais écrits en C++20. CMake remplace Cargo. Les dépendances cryptographiques et de compression sont des bibliothèques natives C ; elles ne requièrent pas Rust.

## Compatibilité conservée

- Identifiants BLAKE3 avec séparation des types, manifestes et révisions v1.
- Frontières FastCDC 2020 identiques à fastcdc 5.0.0, normalisation 1 et seed 0.
- Enveloppes `SYR1`, compression Zstd et chiffrement XChaCha20-Poly1305.
- Configuration et références des stores physiques v1, pointeurs Git et API réseau `/v1`.
- Signatures des fonctions C, types, ownership et codes usuels de succès/erreur.

L'audit `tests/migration_compat.py` a ingéré les mêmes données avec les deux implémentations, comparé les IDs et manifestes, reconstruit avec l'autre binaire, ouvert et écrit les stores de l'autre implémentation. Cela a été effectué en clair et avec chiffrement.

Il a aussi vérifié les révisions et leurs parents avec des chemins Unicode, et les transferts entre client Rust/serveur C++ puis client C++/serveur Rust.

Les fixtures de `tests/fixtures/v1` proviennent du prototype Rust. La clé de leur petit store chiffré est **publique et réservée aux tests**. La suite normale les vérifie sans compiler ou exécuter Rust. L'audit ponctuel peut être relancé si l'ancien exécutable est disponible :

```text
python tests/migration_compat.py build/Release/syrnck.exe chemin/ancien-syrnck.exe
```

## Adaptations explicites

- La commande optionnelle `pack` active le stockage physique v2 pour un seul pack, puis v3 pour plusieurs packs. Le moteur lit v1/v2/v3 ; les anciens binaires refusent les formats physiques inconnus. Les IDs et objets logiques v1 restent inchangés. L'ouverture seule ne convertit pas le store. Une configuration v3 reste v3 même après réduction du nombre de packs. Voir [PACKS.md](PACKS.md).
- Le lecteur paginé conserve les octets et hashes des index v2/v3 existants. Son répertoire, filtre et cache de pages restent en mémoire ; il n'ajoute aucun champ au format ni aucune fonction à l'ABI C. Voir [INDEXES.md](INDEXES.md).

- La validation des collisions de chemins emploie maintenant la normalisation et le casefold Unicode via utf8proc. Elle peut refuser des noms équivalents qu'une simple conversion en minuscules acceptait.
- Les ignore files sont interprétés par le scanner C++ : patterns `*`, `?`, `**`, classes, règles ancrées, dossiers et négations, avec fichiers imbriqués. Les cas de bord doivent encore être éprouvés sur corpus.
- Une exception C++ est convertie en erreur à la frontière C. Le code 2 désigne une exception inconnue ; il désignait auparavant une panic Rust interceptée. Aucun objet C++ ni exception ne doit traverser l'ABI C.
- Le client HTTPS utilise libcurl : Schannel sous Windows, OpenSSL sous Unix. Le serveur HTTP utilise cpp-httplib, reste limité au loopback, et le client n'accepte pas les redirections.

## Construction et distribution

Les cibles principales sont `syrnck_core`, `syrnck_git`, `syrnck_net`, `syrnck`, `syrnck_ffi` et `syrnck_ffi_static`. Pour lier l'ABI C statiquement, définir `SYRNCK_STATIC`. L'édition de liens via les cibles CMake propage les dépendances nécessaires. La bibliothèque C++ statique ne contient pas automatiquement toutes les archives de ses dépendances.

Un [binding Go](../bindings/go/README.md) consomme l'ABI C étendue et est testé localement sous Windows x64. Le moteur n'inclut aucun type Unreal ; l'interface C évite d'exposer les types STL ou JSON aux autres runtimes. L'intégration Unreal est une étape ultérieure, après consolidation du moteur, du CLI et du serveur. Les binaires doivent être construits pour chaque OS/architecture et distribués avec les dépendances système et notices appropriées.

Pour construire uniquement les bibliothèques sans les composants réseau et le CLI, désactiver `SYRNCK_BUILD_NETWORK` et `SYRNCK_BUILD_CLI`. Désactiver aussi `SYRNCK_BUILD_TESTS` et `SYRNCK_BUILD_BENCHMARKS` pour une intégration minimale. Cette configuration ne recherche ni libcurl ni OpenSSL.

Les sources et le binaire Rust initiaux ont été conservés localement sous `.tools/baseline` pour comparaison ; ils sont exclus du projet actif et de sa construction. Les anciens résultats de performance sont conservés et identifiés comme mesures du prototype Rust.
