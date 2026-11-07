---
id: operations
title: Progression et annulation coopérative
description: Guide : Progression et annulation coopérative.
section: Premiers pas
order: 440
sourceRevision: 1
---

# Progression et annulation coopérative {#progress-and-cooperative-cancellation}

Ingestion C++, commit natif, reconstruction et filtres Git acceptent progression et annulation. Commits et clean utilisent les préférences de pipeline du dépôt ; IngestOptions explicite les remplace. Le CLI accepte workers/buffer pour put, commit, add, clean et migration LFS sans modifier les préférences enregistrées.

## CLI {#cli}

Les commandes qui traitent des blocs, métadonnées, snapshots, maintenance, migrations ou transferts acceptent --progress ou --progress-json, mutuellement exclusifs. Les deux écrivent exclusivement sur stderr ; stdout conserve son JSON, ID, pointeur Git ou flux binaire habituel. Sans option, le succès reste silencieux sur stderr. Les commandes sans progression refusent ces options. Un smudge brut ou clean de pointeur existant peut n’émettre aucun événement.

Le texte suit la langue de session et cite les chemins en échappant contrôles/newlines. Le JSON est canonique, un objet par ligne ; total_bytes null si inconnu. Chemins relatifs pour commit, arguments source/destination pour put/get, slash Windows normalisé. Pour une même phase/chemin, environ dix événements par seconde ; changements de portée et fins de fichier toujours émis. Scanning/publication ne sont pas des totaux d’octets projet. Le code d’erreur final prime sur un événement complete antérieur.

LFS preview vérifie ; migration ingère puis publie sa transaction Git sans annulation après engagement. Les [plages](doc:file-ranges) comptent leurs octets relatifs ; [compare](doc:file-comparison) compte des entrées de blocs. La [rétention Git](doc:git-retention) vérifie et publie tous ses pins ensemble, sans retrait automatique. Statut ordinaire et diff sont en lecture seule, sans rapport partiel en échec ; le mode [indexé](doc:working-index) peut écrire un cache jetable. Le [statut combiné et plan de checkout](doc:native-workspace) séparent staged/unstaged, inventorient les collisions et finissent en planning sans publishing. Un plan bloqué est un résultat JSON terminé avec code 1.

Le [checkout](doc:native-checkout) journalise préparation et déplacements. Avant journal, annulation sans changement ; ensuite le workspace peut être partiel avec ancienne branche : inspecter, reprendre ou annuler. Le dernier callback publishing/checkout précède branche/journal atomiques ; après publication, seul le nettoyage continue sans arrêt/callback. Une erreur peut laisser un journal engagé à reprendre, non annulable. GC et écritures natives restent bloqués jusqu’à nettoyage ; cibles préparées et sauvegardes privées sont en clair même pour un Store chiffré. [Switch](doc:native-branches) publie HEAD/journal ensemble en gardant les deux têtes ; abort antérieur garde l’ancienne sélection. native head est une lecture sans progression.

[Tags](doc:native-tags), [staging](doc:native-staging), [reset](doc:native-reset), [squash](doc:native-squash) et sélections textuelles publient leurs groupes de références après fermeture complète vérifiée. Arrêt/exception avant publication garde les anciennes racines, avec objets préparés éventuellement orphelins ; aucun callback après. Staged commit ne lit pas le workspace. Reset soft/mixed et squash ne le modifient pas. Les [archives](doc:native-archives) ne vérifient que les lignes affichées si demandé.

[Merge](doc:native-merge), [replay](doc:native-replay) et rebase planifient sans écrire ; --verify audite entrées et résolutions personnalisées. L’application recalcule le plan vérifié puis utilise le checkout récupérable. Les documents de [résolution](doc:native-resolutions) lient les entrées et le parent choisi ; aucun snapshot partiel n’est publié. Hunks/lignes préservent les autres entrées et archivent l’ancien index ; reset-hunks et reset-lines lisent seulement les snapshots figés. Index périmé ou checkout actif bloque. Le [patch externe](doc:text-patches) ne lit pas le workspace et lie octets, source, limites et résultat au plan attendu ; erreur de patch/budget/contexte avant ingestion sans référence changée.

Ctrl+C, Ctrl+Break Windows et SIGINT/SIGTERM disponibles demandent l’arrêt coopératif, code 130 et diagnostic traduit. Le handler ne note qu’un flag atomique ; un thread normal demande le stop C++. Pipe/périphérique bloqué doit rendre la main ; les interruptions répétées demandent le même arrêt. La terminaison forcée suit la [récupération](doc:crash-recovery), distincte. Le nettoyage de maintenance engagé se termine sans nouvelle annulation.

```text
syrnck config set --workers 4 --buffer-mib 32
syrnck put Content/Map.uasset --progress
syrnck native commit -m "Snapshot" --progress-json
syrnck get MANIFEST restored.uasset --progress
```


```json
{"phase":"ingesting","path":"Content/Map.uasset","bytes":1048576,"total_bytes":8388608,"complete":false,"buffered_blocks":2}
```


## Opérations C++ {#c-operations}

L’observateur reçoit ProgressEvent synchronement sur le coordinateur. Phases : scanning, ingesting, reconstructing, verifying, exporting, transferring, planning, packing, publishing. Événement : octets traités, total optionnel, chemin optionnel, complete pour cette portée, blocs tamponnés. Commit fournit chemin relatif/taille observée ; un flux ordinaire n’a pas de total connu ; les fichiers vides rendent aussi fin/progression.

Les chemins sont empruntés durant le callback, à copier si conservés. Ne pas réentrer ou modifier le Store. Une exception arrête le travail et joint les workers. Les octets d’entrée peuvent être encore en file, sans preuve d’écriture durable.

Arrêt vérifié entre lectures, blocs, dossiers et publications ; IO/codecs déjà lancés peuvent finir. Les workers encodent, le coordinateur écrit et appelle les observateurs. Un import arrêté peut laisser blocs et manifeste complet non référencés. Une fin d’ingestion concerne le fichier, pas une branche/pin/destination.

Les commits vérifient leur fermeture avant déplacement de branche et contrôlent le stop jusqu’à l’écriture atomique. set_ref, compare_set_ref et closure acceptent aussi un stop_token. Avant engagement, ancienne référence intacte ; un stop concurrent au point atomique peut arriver trop tard.

reconstruct vers un flux appelant peut laisser une sortie partielle. restore_file prépare un frère temporaire, contrôle l’arrêt avant publication exclusive et nettoie à l’annulation ; aucune destination existante n’est écrasée. Les filtres required signalent l’échec ; clean n’émet le pointeur qu’après rétention du manifeste.

fsck(OperationOptions) vérifie en flux sans liste complète d’objets. Les octets décodés incluent répétitions nécessaires aux hashes et copies physiques secondaires ; total inconnu, fin seulement après succès. Arrêts entre objets/blocs/entrées ; validation format/index en cours peut finir. Échec ferme les sessions, rétablit le cache et ne modifie pas les données ; aucun faux succès stdout. closure garde, elle, son résultat matérialisé. Export multi-fichier prépare un arbre complet avant publication exclusive.

Le [réseau](doc:transfers) possède son point de CAS distant et son code 3 incertain. Journaux/baux/inventaires conservent sources ou objets reçus pendant GC ; prune ne retire que les reçus complets. [Maintenance](doc:maintenance) s’annule pendant vérification/plan/construction/relecture, puis termine le nettoyage engagé sans callback. Les formats d’objet/pointeur/ABI ne changent pas ; les baux négocient des extensions HTTP. Recompiler les consommateurs C++ avec leurs en-têtes.

Les [diffs textuels](doc:text-diff) reconstruisent seulement le chemin demandé, refusent les budgets avant lecture, distinguent binaire/limites et ne publient pas de patch partiel. Le diff de travail contrôle ignores, obstructions et mutations observées ; une égalité inconnue reste null. Hunks, lignes et patches ont leurs propres refus de résultats surdimensionnés ou assemblages de newlines invalides, sans normaliser le workspace.

```cpp
std::stop_source cancellation;
auto options = store.settings().ingest_options();
options.stop = cancellation.get_token();
options.progress = [&](const syrnck::ProgressEvent &event) {
  // Observe progress here; another thread may call cancellation.request_stop().
};
auto revision = store.commit_directory(project, "main", "Snapshot", options);
```
