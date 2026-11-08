---
id: resident-agent
title: Agent résident de publication native
description: Surveiller un workspace natif, capturer les changements et reprendre les publications protégées.
section: Transfers
order: 405
parent: transfers
sourceRevision: 1
---

## Fonctionnement continu {#run}

```text
syrnck agent run https://blocks.example.invalid --directory PROJECT --remote-branch main --require-lease
syrnck --workspace edit agent run https://blocks.example.invalid --remote-branch feature --require-lease
syrnck agent run https://blocks.example.invalid --commits-only --polling
```

Ces exemples nécessitent un stockage initialisé, un vrai serveur et `SYRNCK_TOKEN`.
`--store PATH` sélectionne le stockage. Sans `--directory`, le workspace principal
utilise le dossier courant ; un workspace lié sélectionné utilise son dossier
enregistré. HEAD, nommé ou détaché, reste figé pour la session. `--branch NAME`,
si fourni, doit lui correspondre. La branche distante vaut `main` par défaut ;
les noms distants sont des composants uniques d’au plus 64 octets.

Par défaut, l’agent observe les fichiers matérialisés et crée un snapshot natif
quand leur contenu change, y compris suppressions et renommages exacts. Un
workspace partiel conserve les fichiers non matérialisés de sa révision parente
complète. Plusieurs fichiers ne constituent pas un snapshot atomique du système
de fichiers : stabiliser les écrivains externes pour capturer un état applicatif
cohérent. `--commits-only` publie les commits locaux existants sans créer de
révision ni modifier les fichiers de travail sales. L’agent ne remplace jamais
ces fichiers par du contenu distant.

Il s’agit du sens résident de **publication**, pas encore d’un service complet
de synchronisation bidirectionnelle de dossiers. Un descendant distant ou des
historiques concurrents demandent une intervention ; aucune copie conflictuelle
ou fusion n’est choisie silencieusement. Réconciliation causale complète,
installation comme service système et interopérabilité complète avec un daemon
Syncthing restent à terminer. Voir les [limites actuelles](doc:limits).

## Publication et récupération {#recovery}

Chaque publication exige la révision distante observée comme valeur attendue.
Fermetures complètes vérifiées, [sessions de transfert](doc:transfer-sessions),
permissions, clés de signature approuvées et éventuelles preuves de verrous
d’assets s’appliquent comme aux transferts ordinaires. `--require-lease` exige la
protection négociée contre le GC distant ; l’agent n’acquiert ni ne renouvelle
automatiquement les verrous d’assets. Fournir les preuves actuelles avec
`--lock-claims PATH` lorsque la politique de branche les exige.

Une publication réussie conserve une racine typée de référence et un descripteur
sous les références `sync/push/SERVICE/` du workspace sélectionné. Le descripteur
lie workspace logique, cible HEAD, branche distante et empreinte de l’adresse
normalisée. Il ne contient ni jeton ni ancienne destination physique. Les états
remplacés restent protégés par `archive/agent-push/SERVICE/REVISION`. GC et
sauvegardes logiques conservent ainsi toutes les dépendances de reconstruction.
Les interrogations et redémarrages sans changement ne réécrivent pas le point
de reprise.

Une suppression ou un recul distant après publication reconnue est bloqué au
lieu d’être annulé. Une réécriture locale est également bloquée. Une révision
distante inconnue est récupérée dans une référence distincte
`remotes/agent-…/BRANCH`, sans changer HEAD ni les fichiers. Un ancêtre distant
de HEAD peut être avancé ; descendants distants et historiques divergents sont
bloqués.

Si une publication précédente a une issue inconnue, sa session persistante
d’origine doit d’abord se réconcilier, même si HEAD local a progressé. Un reçu
de succès passé n’autorise pas à écraser un changement distant ultérieur. Un
conflit enregistré demande une résolution explicite ; l’agent ne supprime jamais
automatiquement ce journal. Après résolution du problème d’historique ou de
politique, il réessaie. Un checkout incomplet ou un index de préparation occupé
bloque capture et publication jusqu’à sa résolution.

Des changements hors ligne peuvent toujours devenir des snapshots locaux signés.
La reconnexion transfère leurs blocs manquants. Le redémarrage rescane le
workspace et réutilise révisions, points de reprise et sessions persistantes.
L’identité de signature est chargée une fois au démarrage et gardée en mémoire ;
changer le profil ou faire tourner sa graine exige de redémarrer l’agent. Voir
la [configuration d’identité](doc:native-authorship).

## Planification et sortie {#controls}

| Option | Défaut | Valeurs acceptées |
| --- | --- | --- |
| `--interval-ms` | 500 | 20..60000 |
| `--debounce-ms` | 75 | 0..5000 |
| `--audit-ms` | 300000 | 1000..3600000 |
| `--remote-poll-ms` | 5000 | 100..3600000 |
| `--duration-ms` | 0 | 0 pour continu, ou 100..86400000 |
| `--max-updates` | 0 | 0 pour illimité, ou 1..1000000 |
| `--max-paths` | 4096 | 1..65536 |

Les notifications natives sont indicatives ; audits périodiques et rescans au
redémarrage récupèrent la perte de continuité. `--polling` utilise des rescans
prudents. Une erreur est réessayée après une seconde. Le stockage est libéré
avant attente, regroupement d’événements et écriture d’un état : les autres
commandes CLI peuvent passer entre les cycles. Scans et transferts réseau
gardent son verrou exclusif ; un transfert volumineux peut retarder les commandes.

La sortie standard produit des lignes NDJSON lorsque l’état sémantique change.
`state` vaut `empty`, `pushed`, `synced`, `uncommitted`, `blocked` ou `unavailable`.
Les codes de raison distinguent index, checkout, réécritures, références
supprimées, historiques concurrents et publications incertaines. Les lignes
incluent les révisions locales/distantes et la référence reconnue si connues.
`working_clean: null` demande un nouvel audit, notamment juste après un snapshot
automatique. Les messages suivent `--lang en|fr` ; IDs et clés JSON restent
identiques. `--progress` ou `--progress-json` écrit la progression sur stderr.

À une limite de durée ou de mises à jour, le code de sortie reflète le dernier
cycle : 0 pour un état ordinaire, 1 pour une opération indisponible, 2 pour un
blocage, 3 pour une publication incertaine. L’annulation coopérative renvoie 130.
La durée limite la planification, sans interrompre immédiatement les IO en cours.

## Intégration C++ {#cpp}

```cpp
#include <syrnck/native_sync.hpp>

syrnck::net::NativePushSync sync(store, project, {"main", true});
auto status = sync.tick(store, remote, watcher_changes, operation, transfer);
```

Conserver cette session déplaçable et son observation entre les cycles. Chaque
cycle utilise un stockage actuellement verrouillé, avec mêmes stockage physique,
workspace logique, cible HEAD et adresse distante. Appeler `invalidate()` après
perte de continuité du watcher ; une nouvelle session commence par un audit
complet. L’appelant gère planification et annulation. Cette API ne lance aucun
thread de fond et ne charge pas implicitement le profil CLI. Lier `syrnck_net` et
fournir explicitement l’identité éventuelle. La CLI utilise
`Store::take_native_identity()` pour déplacer la propriété exclusive de la clé
entre handles courts sans exporter les octets de la graine.
