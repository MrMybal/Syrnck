---
id: native-replay
title: Cherry-pick et revert
description: Guide : Cherry-pick et revert.
section: Versionnage
order: 330
parent: native-history
sourceRevision: 1
---

# Cherry-pick et revert natifs {#native-cherry-pick-and-revert}

Le plan applique le delta de fichiers d’un commit à la branche : parent vers source pour cherry-pick, source vers parent pour revert. Les changements ultérieurs indépendants de la cible peuvent être conservés. Le plan n’écrit rien.

--mainline N choisit un parent à partir de 1, explicitement requis pour une fusion. Commit ordinaire : parent 1 ; racine : arbre vide et option mainline refusée. Sélecteurs au CLI, IDs exacts en C++. Plan prêt : 0 ; vide, conflictuel ou bloqué : 1 avec JSON ; entrée/métadonnée invalide : diagnostic stderr.

```text
syrnck native cherry-pick-plan tags/fix --verify
syrnck native revert-plan COMMIT_ID --branch main --directory .
syrnck native revert-plan MERGE_ID --mainline 2
```


## Décisions à trois voies {#three-way-decisions}

Base, branche actuelle et entrant sont comparés par hash complet/taille/mode. Rechunking seul n’est pas une édition. Changements identiques ou entrant inchangé gardent le manifeste actuel ; un seul côté changé est retenu ; contenu et mode indépendants d’un même fichier peuvent se combiner.

Conflits : content, add_add, delete_modify, modify_delete ; choix explicites via --resolutions, composition textuelle optionnelle. La fusion des renommages reste hors de ces règles. Les ajouts indépendants sont contrôlés ensemble pour case_collision et file_directory_collision.

Rapport : operation, branch, head attendue, source, parent/mainline, ready, empty, objects_verified, result_files, changes, conflicts, resolved, resolution_context. Résumés complets manifeste/hash/taille/mode ; conflits avec les trois côtés, namespace avec chemin opposé. Un seul conflit bloque tout.

Sans conflit, workspace contient le préflight habituel : suivis propres, ignorés/non-suivis, protections, liens, collisions, staging et récupération. Sa target reste null sans candidat écrit. Sans delta logique, empty true et non prêt ; avec conflits, workspace null. --verify contrôle les deux histoires et fichiers complets ; sinon seules métadonnées et fichiers de travail, pas blocs entrants/personnalisés hors composition textuelle.

JSON compact limité à 16 Mio ; candidat réservé conservativement avec 4 Kio pour en-tête/message. Mémoire selon trois snapshots, résultat et rapport ; pas d’arbre en flux. Les longs messages gardent la limite de révision. C++/CLI seulement, callbacks non réentrants, progression/erreurs bilingues et raisons JSON stables.

```cpp
auto plan = store.plan_replay(project, "main", selected_id,
                              syrnck::NativeReplayKind::cherry_pick, {}, true, operation);
auto inverse = store.plan_replay(project, "main", merge_id,
                                 syrnck::NativeReplayKind::revert, 2, false, operation);
```


## Composition textuelle bornée optionnelle {#optional-bounded-text-composition}

plan_replay_with_options accepte NativeReplayOptions (mainline, resolutions, text_merge), avec anciennes entrées compatibles et mêmes défauts. Sans text_merge, comportement par fichier inchangé.

Choix explicites d’abord ; conflits content de fichiers existants seulement. Pour cherry-pick, base textuelle = parent choisi, entrant = source ; pour revert, base = source, entrant = parent choisi ; côté courant = branche. Cette inversion retire le delta tout en gardant les changements ultérieurs indépendants. Racines, ajouts/suppressions, namespace, binaire et chevauchements gardent leurs conflits.

text_merge null si désactivé ; sinon mêmes limites/compteurs/statuts/plages/métadonnées que merge. Trois entrées reconstruites et hashées à chaque tentative même sans audit d’histoire ; plafonds avant lecture, au plus 64 conflits et 16 Mio agrégés entrées/résultats propres, en plus des limites par fichier.

Les résultats propres possèdent des IDs v1 prédits et octets candidats internes, sans stockage ni texte brut dans le plan. Une résolution tout-current peut rester vide ; tout conflit bloque l’ensemble. Tous les contrôles workspace/index/journal/mainline/contexte restent valables.

replay_directory_with_options exige la tête attendue, recalcule et vérifie les deux fermetures avant toute matérialisation. Vide/conflit/index/obstacle ne matérialisent rien. Manifestes générés conformes aux IDs prédits avant révision à un parent. Le journal retient le candidat complet pour resume/abort ; avant journal, seuls des objets non référencés peuvent rester, pas de changement de branche/disque. Le manifeste final conserve directement ses blocs sans dépendre de l’histoire source sans lien ; garder sa branche/tag/pin si cette histoire doit aussi survivre.

CLI : --text-merge et max-file-bytes/max-lines/max-work/max-output-bytes, ces limites exigeant l’activation. Même options au plan/application. Mainline, résolutions, tête attendue, message requis et progression gardent leur sens ; tout conflit/budget restant bloque tous les fichiers générés.

```cpp
syrnck::NativeReplayOptions settings;
settings.text_merge.emplace(); // Default per-file text limits.
// settings.mainline = 2; // Required for a selected merge parent.
auto plan = store.plan_replay_with_options(project, "main", source_id,
    syrnck::NativeReplayKind::cherry_pick, settings, true, operation);
```


```cpp
auto applied = store.replay_directory_with_options(project, "main", expected_id,
    source_id, syrnck::NativeReplayKind::revert, "Undo selected changes",
    settings, operation);
```


```text
syrnck native cherry-pick-plan feature --text-merge --verify
syrnck native cherry-pick feature --text-merge --expect OLD_TIP_ID -m "Apply fix"
syrnck native revert-plan SOURCE_ID --text-merge --max-work 33554432
syrnck native revert SOURCE_ID --text-merge --expect OLD_TIP_ID -m "Undo fix"
```


## Appliquer un changement {#apply-a-planned-change}

Message obligatoire. Branche/dossier/sélecteurs comme le plan ; --expect est un ID exact minuscule, sinon capture sous verrou au CLI, toujours requis par l’API.

L’application recalcule et vérifie histoires et fichiers ; elle refuse conflit, résultat vide, récupération checkout/switch et tout index. Tous les suivis et destinations passent clean-tracked, sans force. Le candidat v1 utilise message, date UTC courante et seul parent ancienne tête cible ; cherry-pick n’ajoute pas source comme parent de fusion. Une source fusionnée exige mainline mais produit toujours un parent.

Le checkout récupérable change les fichiers avant publication atomique branche/archive/reçu ; la branche garde l’ancienne tête jusqu’à vérification de toutes les sorties. Ancienne tête/histoire/blocs restent accessibles par parent et archive. HEAD et références source restent inchangés. Une source sans lien nécessite sa propre racine pour garder toute son ascendance.

La réponse directe ajoute operation, replayed_revision, previous_revision et mainline aux champs du reçu checkout (target/state/archive). La récupération rend l’identité générique et sa cible, sans ces labels de réponse. Formats inchangés.

```text
syrnck native cherry-pick SOURCE_ID -m "Apply the asset fix" --expect OLD_TIP_ID
syrnck native revert SOURCE_ID -m "Undo the asset fix"
syrnck native revert MERGE_ID --mainline 2 -m "Undo the selected merge delta"
```


```cpp
auto applied = store.replay_directory(project, "main", expected_id, source_id,
                                      syrnck::NativeReplayKind::cherry_pick,
                                      "Apply selected changes", {}, operation);
```


## Interruption, annulation et rétention {#interruption-abort-and-retention}

Avant journal, arrêt/erreur conservent branche et workspace mais peuvent laisser un candidat immuable non référencé. Après journal, inspecter puis checkout-recover ou --abort. Resume installe le candidat enregistré sans recalculer le replay. Abort rétablit les fichiers originaux et garde la branche avant publication ; après publication, seul le nettoyage peut finir.

Ouvrir le Store ne change aucun fichier et le GC reste bloqué tant que le journal est actif. Inspecter la tête après perte de réponse ; l’ancien --expect empêche un second commit. Les replays vides sont refusés.

Révisions finales et parents survivent au GC. Un candidat non publié et abandonné peut être collecté après retrait des pins temporaires. Archives/reçus checkout n’expirent pas automatiquement, leur [retrait protégé](doc:checkout-retention) est explicite. Fichiers préparés et sauvegardes privées restent en clair malgré le chiffrement des objets ; voir les [limites du checkout](doc:native-checkout).

```text
syrnck native checkout-status
syrnck native checkout-recover
# Or, before branch publication:
syrnck native checkout-recover --abort
```
