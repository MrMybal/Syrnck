---
id: resident-agent
title: Agent résident de synchronisation native
description: Surveiller les workspaces, échanger les révisions protégées et fusionner les historiques compatibles.
section: Transfers
order: 405
parent: transfers
sourceRevision: 9
---

## Fonctionnement continu {#run}

```text
syrnck agent run https://blocks.example.invalid --directory PROJECT --remote-branch main --require-lease
syrnck --workspace edit agent run https://blocks.example.invalid --remote-branch feature --require-lease
syrnck agent run https://blocks.example.invalid --commits-only --polling
syrnck agent run https://blocks.example.invalid --bidirectional --require-lease --require-read-lease
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
cohérent. `--commits-only` désactive les snapshots automatiques des fichiers.
Dans le sens de publication par défaut, il publie uniquement les commits
existants sans changer les fichiers. En mode bidirectionnel, les historiques
existants peuvent encore produire une révision de fusion comme décrit ci-dessous.

Synchronisation entièrement autonome de dossiers, installation
comme service système et interopérabilité complète avec un daemon Syncthing
restent à terminer. Voir les [limites actuelles](doc:limits).

## Échanger les révisions dans les deux sens {#bidirectional}

`--bidirectional` autorise explicitement les changements entrants des fichiers.
Un workspace vide et propre peut recevoir sa première révision distante. Les
descendants distants s’appliquent par la transaction de checkout récupérable,
qui conserve l’ancien état et respecte matérialisation partielle et HEAD détaché.

Des descendants concurrents d’un historique partagé utilisent une fusion à trois
voies. Les changements de fichiers compatibles et éditions textuelles sans
chevauchement peuvent produire une révision signée à deux parents, publiée
contre la racine distante exactement observée. La composition textuelle utilise
le moteur borné existant, avec au plus 64 essais et 16 Mio d’entrées/résultats par
lot. Les [bases communes multiples](doc:native-merge#recursive-bases) utilisent une synthèse récursive bornée. Les chemins incertains divergents nécessitent une résolution ou des copies explicites. Les historiques sans lien ne sont pas fusionnés automatiquement. Une réécriture distante hors de l’historique reconnu est bloquée
même si une fusion pourrait autrement la combiner.

Les conflits binaires, textuels chevauchants ou topologiques non résolus bloquent
l’agent avec le plan de fusion. Historiques local et distant récupéré restent
conservés ; les octets de travail sont préservés. Résoudre avec le
[parcours de fusion native](doc:native-resolutions) ; l’agent réessaie l’historique
local résultant. La politique optionnelle ci-dessous peut conserver
automatiquement les versions concurrentes des fichiers.

Avec `--commits-only --bidirectional`, réception et fusion exigent toujours un
workspace propre : les fichiers sales sont conservés et bloquent l’application.
Une édition arrivant pendant le précontrôle peut aussi bloquer le checkout.
Les indications du watcher n’autorisent jamais un remplacement sans vérification.

## Conserver les assets conflictuels {#conflict-copies}

```text
syrnck agent run https://blocks.example.invalid --bidirectional --conflict-copies --require-lease
```

`--conflict-copies` exige `--bidirectional` et reste désactivé par défaut. Après
les fusions compatibles/textuelles normales, les modifications binaires ou
textuelles chevauchantes et les ajouts concurrents conservent les deux versions
dans le snapshot de fusion. L’ordre stable des IDs de révisions sélectionne le
chemin initial ; une copie voisine déterministe conserve l’autre version et les
extensions d’assets courantes. Suppression/modification garde le fichier modifié.
Consulter les [règles exactes de nommage et collision](doc:native-merge#conflict-copies).

`merge.resolved` identifie chaque copie ou modification conservée. Les copies
sont suivies et transférées par les mêmes manifests vérifiés, publication
protégée et révision signée à deux parents. Inverser les pairs propose le même
ensemble de fichiers ; un descendant résolu ne crée pas de nouvelles copies.
Examiner les copies puis committer le choix final. Conflits de structure,
noms générés occupés, obstacles non suivis, historiques sans lien/réécrits,
index et récupération incomplète continuent de bloquer. Un candidat déjà préparé
reprend à l’identique même si l’option est omise après redémarrage.

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
de HEAD peut être avancé. Le sens de publication par défaut bloque descendants
distants et historiques divergents ; `--bidirectional` permet la réception et
les fusions compatibles décrites ci-dessus.

Si une publication précédente a une issue inconnue, sa session persistante
d’origine doit d’abord se réconcilier, même si HEAD local a progressé. Un reçu
de succès passé n’autorise pas à écraser un changement distant ultérieur. Un
conflit enregistré demande une résolution explicite ; l’agent ne supprime jamais
automatiquement ce journal. Après résolution du problème d’historique ou de
politique, il réessaie. Le mode bidirectionnel peut [reprendre son propre checkout interrompu](doc:native-checkout#owned-recovery)
hors ligne sans signer à nouveau sa cible figée. Les checkouts manuels, d’autres
sessions ou en mode publication seule restent bloqués. Un index de préparation
occupé bloque capture et publication jusqu’à sa libération.

La référence typée `sync/push/SERVICE/observed` conserve le dernier historique
distant confirmé avant application des fichiers et avant de terminer une session
d’envoi. Les observations précédentes restent sous `archive/agent-observation/`.
Suppression ou recul de cet historique est bloqué même si un crash précède le
point de reprise reconnu. Une nouvelle session peut importer les reçus natifs
complets encore disponibles comme faits historiques, sans autoriser l’écrasement
d’un changement distant ultérieur. `resident-sync-v1` protège cet état avant
publication et reste activé après nettoyage ou changement de mode.

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
`state` vaut `empty`, `pushed`, `pulled`, `recovered`, `synced`, `uncommitted`, `blocked` ou `unavailable`.
Les codes de raison distinguent index, checkout, réécritures, références
supprimées, historiques concurrents et publications incertaines. Les lignes
incluent les révisions locales/distantes et la référence reconnue si connues.
`last_observed_remote` expose l’historique conservé avant même ce point de reprise.
`merged: true` signale une fusion locale automatique ; les rapports checkout/merge
décrivent l’application entrante ou ses blocages lorsque nécessaire.
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

syrnck::net::NativeSync sync(store, project, {"main", true, true, true});
auto status = sync.tick(store, remote, watcher_changes, operation, transfer);
```

Conserver cette session déplaçable et son observation entre les cycles. Chaque
cycle utilise un stockage actuellement verrouillé, avec mêmes stockage physique,
workspace logique, cible HEAD et adresse distante. Appeler `invalidate()` après
perte de continuité du watcher ; une nouvelle session commence par un audit
complet. L’appelant gère planification et annulation. Cette API ne lance aucun
thread de fond et ne charge pas implicitement le profil CLI. Les options sont
branche distante, capture locale, réception, fusions compatibles et copies de
conflit ; les trois dernières valent false par défaut. La fusion exige la
réception et les copies exigent les deux. Par exemple,
`NativeSyncOptions{"main", true, true, true, true}` active la conservation. Les noms
`NativePushSync` / `NativePushSyncOptions` restent valides. Lier `syrnck_net` et
fournir explicitement l’identité éventuelle. La CLI utilise
`Store::take_native_identity()` pour déplacer la propriété exclusive de la clé
entre handles courts sans exporter les octets de la graine.

## Éditions pendant une application à propriétaire {#active-checkout-edits}

La politique existante explicite `--conflict-copies` conserve aussi les éditions
de fichiers ordinaires survenues pendant l’application. L’agent retient des
archives natives avant les déplacements exclusifs vers des copies de conflit,
restaure les octets intermédiaires attendus et termine le candidat entrant/fusionné
inchangé. Cette reprise fonctionne hors ligne sans clé privée pour une cible
signée préparée. La capture locale suivante enregistre les fichiers ajoutés.
Sans cette politique, la reprise reste stricte. Fichiers manquants, obstructions
de dossiers/liens et structures ambiguës demandent encore une résolution. Voir le
[contrat partagé d’archive et de reprise](doc:syncthing#active-checkout-edits).

## E/S sources des captures {#incremental-capture}

Les captures répétées réutilisent les manifestes de session inchangés grâce aux
empreintes fiables des fichiers locaux. Inventaire sélectionné et vérification
de toute la fermeture restent requis. Le rapport de tick expose les compteurs
sources `capture`. Voir [performance et repli](doc:performance#incremental-capture).

## Renommages modifiés dans les fusions automatiques {#edited-renames}

`agent run URL --bidirectional --rename-similarity 50` active la
[fusion avec renommages](doc:native-file-history#rename-merges) après récupération
de l'historique divergent. Les budgets `--rename-max-*` s'appliquent. Les sessions
C++ règlent `NativeSyncOptions.renames` ; les fusions bidirectionnelles sont
requises. Le texte compatible suit la destination ; les conflits structurels
restent bloqués. Identité, signatures, branches et chemins gardent leurs
contrôles. Un checkout préparé reprend sa cible figée même si le processus
suivant omet l'option.

Le [fournisseur de signature protégé](doc:signing-keys#services) authentifie une fois au démarrage et conserve le signataire natif développé entre sessions courtes. Les changements d’identifiants demandent un redémarrage ; un fournisseur configuré indisponible empêche ce démarrage.
