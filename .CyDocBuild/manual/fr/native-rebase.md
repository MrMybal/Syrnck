---
id: native-rebase
title: Rebase natif
description: Rebase linéaire et avec préservation des fusions.
section: Versionnage
order: 310
parent: native-history
sourceRevision: 4
---

# Rebase natif {#native-linear-rebase}

Par défaut, le plan rejoue la plage explicite (BASE, HEAD] sur une autre révision. BASE est exclue et doit être atteinte en suivant les parents uniques de la branche ; aucune base commune implicite. Une [séquence éditable/interactif](doc:native-rebase-todo) peut réordonner, retirer, reformuler, squash/fixup ; sans séquence, les règles ordinaires ci-dessous s’appliquent.

Sélecteurs usuels au CLI, IDs exacts en C++. Branche sélectionnée par défaut, --branch et --directory dans les contraintes de liaison. Workspace avec tous suivis propres, aucun non-suivi/ignoré obstructif, aucun index ni récupération checkout/switch.

```text
syrnck native rebase-plan OLD_BASE --onto upstream --verify
syrnck native rebase-plan OLD_BASE --onto NEW_BASE --max-commits 500
```


## Rejeu des fichiers et topologie {#file-replay-and-topology}

Ordre du plus ancien au plus récent : parent original comme base, résultat virtuel précédent comme current et commit original comme incoming. Changements indépendants de fichier/contenu/mode combinés. Conflit de contenu, ajout/suppression ou namespace arrête au premier commit, sans traiter les suivants comme appliqués. Composition textuelle facultative ; le [rejeu avec renommages](doc:native-file-history#rename-merges) s’active avec `--rename-similarity 50`.

Fusion dans la plage : merge_in_range ; fusion à la borne exclue autorisée. Racine atteinte avant BASE : base_not_on_linear_history. Aucun choix arbitraire ni aplatissement silencieux de parents de merge. Défaut 1000 commits, réglable 1..100000 ; dépassement échoue.

BASE = ONTO sur plage valide : à jour et IDs exacts conservés. HEAD = BASE : plage vide proposant le déplacement sur ONTO. Sinon chaque commit garde son message et reçoit un seul nouveau parent virtuel. Sans identité, les dates anciennes restent conservées ; avec identité, auteur d’origine conservé et committer actuel avec une date commune figée. Les replays logiquement vides sont conservés comme commits ; changement de représentation seul n’est pas une édition de contenu.

## Rapport et bornes {#read-only-report-and-bounds}

JSON : head/base/onto/mode, commits ordonnés, steps, completed_commits, target proposée, workspace, ready, blockers. Une étape décrit source/parent d’origine, head virtuelle, target, empty, changements et conflits. Prêt 0 ; bloqué 1 avec JSON ; erreurs d’options/métadonnées en diagnostic. Plage bloquée/conflit : target/workspace null.

Les IDs virtuels suivent l’algorithme de révision ordinaire et la validation complète des métadonnées. Aucun candidat n’est écrit ni fichier/référence déplacé ; un ID proposé peut donc être absent du Store. Avec identité, `--committer-time SECONDS` fige la date exposée par `committer_timestamp` ; sinon des invocations distinctes peuvent choisir des dates et IDs différents. Mêmes entrées, identité, clé et date donnent les mêmes IDs. Sans identité, les dates anciennes sont conservées. --verify audite toute l’histoire head/base/onto et les fichiers ; sans cela, pas d’audit de blocs hors fichiers éligibles aux tentatives textuelles. Chaque étape rend resolution_context vide et resolved ; le contexte global est un modèle de série.

Rapports et chaque révision plate limités à 16 Mio. Le plan garde liste d’IDs/rapport bornés et snapshots courants, pas toutes les tables candidates ; il peut relire les métadonnées. Beaucoup de changements par étape peuvent atteindre le plafond avant le compte de commits. C++/CLI, progression bilingue, annulation coopérative et callbacks non réentrants. Les bindings d’historique C/Go restent à développer ; l’édition interactive couvre les modes linéaire et préservation des fusions décrits ci-dessous.

```cpp
syrnck::NativeRebaseOptions settings;
settings.max_commits = 500;
auto plan = store.plan_rebase(project, "feature", old_base, new_base,
                              settings, true, operation);
```


## Appliquer et récupérer {#apply-and-recover}

L’application recalcule/vérifie tout le plan prêt avant stockage, puis répète le calcul déterministe et écrit les candidats complets du plus ancien au plus récent, exigeant l’ID final prédit. Elle ne garde pas toutes leurs tables en mémoire ; révisions v1 ou v2 immuables. Les commits vides suivent les [règles d’auteur/committer](doc:native-authorship#rewrites). La récupération utilise les candidats figés sans clé privée de signature.

Seul le dernier candidat est installé via checkout récupérable ; aucune étape intermédiaire ne bouge branche ou fichiers. L’archive checkout protège l’ancienne tête et tout l’historique d’origine ; la nouvelle chaîne protège ONTO et chaque résultat rejoué, même intermédiaire après GC. Retrouver l’ancienne histoire avec native archives --kind checkout. Branches/tags source et HEAD ne changent pas.

--expect exact, capturé sous verrou si absent au CLI et toujours requis en C++. Up-to-date rend unchanged sans écriture mais vérifie données et workspace. Plage vide restaure ONTO exact sans commit. Conflit, index, suivis sales et histoire incomplète bloquent.

Avant journal, annulation conserve branche/workspace, avec candidats complets orphelins possibles. Après, inspecter et reprendre/annuler explicitement ; abort seulement avant publication de branche, ensuite nettoyage. Ouverture sans récupération automatique ; journal actif bloque GC/mutations. Préparations et sauvegardes privées restent en clair.

Réponse directe : operation rebase, mode, previous_revision, base, onto, replayed_commits plus reçu checkout. Reprise : journal/cible génériques. Formats inchangés et racines sans expiration automatique.

```text
syrnck native rebase OLD_BASE --onto upstream --expect OLD_TIP_ID
syrnck native checkout-status
syrnck native checkout-recover
syrnck native checkout-recover --abort
```


```cpp
auto applied = store.rebase_directory(project, "feature", expected_id,
                                       old_base, new_base, settings, operation);
```


## Résoudre par étape {#resolve-conflicts-by-step}

--resolutions prend le contexte global du plan dans un document puis, pour chaque conflit, son contexte d’étape dans steps avec files complété. Garder les anciens choix et replanifier avec mêmes BASE/ONTO ; le modèle global repart toujours avec steps vide.

Wrapper exact : version 1, head d’origine, base exclue, onto, steps. Chaque entrée du schéma de résolution utilise operation rebase, source commit original, base/parent parent original, head parent virtuel courant : ONTO d’abord, candidat précédent ensuite. Utiliser les IDs/contextes rendus plutôt que les fabriquer.

Choix current/incoming/base/remove/file avec mode explicite, limités aux conflits de l’étape. Namespace revérifié ; résolution partielle bloque sans candidat/fichier écrit. Une étape devenue vide reste un commit avec message/date d’origine. Manifestes personnalisés audités par --verify et toujours à l’application ; garder leur pin si GC possible.

Changer head/BASE/ONTO rejette le wrapper. Modifier un choix antérieur change les IDs suivants et rend leurs contextes périmés. Source étrangère, étapes dupliquées, clés JSON dupliquées, champs inconnus et choix invalides échouent. Les étapes tardives sont contrôlées à leur arrivée ; aucun préfixe conflictuel n’est publié.

Document 16 Mio, 100000 étapes et 100000 choix de fichiers au total ; décodage brut profondeur 64, 1100010 champs totaux. Les décodeurs d’opération gardent leurs limites propres. NativeRebaseResolutionSet range les étapes par ID source.

```text
syrnck native rebase-plan OLD_BASE --onto upstream --resolutions choices.json --verify
syrnck native rebase OLD_BASE --onto upstream --resolutions choices.json --expect OLD_TIP_ID
```


```cpp
auto choices = syrnck::NativeRebaseResolutionSet::parse(plan["resolution_context"]);
auto step = syrnck::NativeResolutionSet::parse(plan["steps"][0]["resolution_context"]);
step.files["Content/Map.uasset"] = {syrnck::NativeResolutionChoice::incoming};
choices.steps[step.source] = step;
settings.resolutions = choices;
```


## Composition textuelle bornée {#optional-bounded-text-composition}

--text-merge tente les conflits content UTF-8 après les choix explicites, en conservant fins de ligne, absence de newline finale et modes indépendants. Binaire, chevauchement, ajouts/suppressions et namespace restent bloquants. Aucun marqueur de conflit ni histoire partielle publiée ; commits vides conservés.

Un lot couvre toute la plage : 64 tentatives maximum et 16 Mio entrées/résultats propres, plus max-file-bytes/max-lines/max-work/max-output-bytes par fichier. Relire un ancien résultat virtuel le refacture ; les budgets ne repartent pas par commit et leur épuisement bloque tout. Les options de budget exigent --text-merge.

text_merge global expose compteurs cumulés/limites. Les fichiers par étape décrivent ses tentatives et leurs statuts/motifs/coordonnées/manifestes, avec compteurs cumulatifs, sans texte brut. Choix explicites prioritaires sans tentative automatique. Garder options/budgets/choix précédents pour des résolutions ultérieures : toute modification change la tête virtuelle attendue.

Un cache mémoire borné garde les contenus générés pour les étapes suivantes sans stockage. IDs CDC et révisions restent reproductibles. L’application vérifie tout le plan/workspace avant de matérialiser tous les manifestes générés, intermédiaires compris, écrire la chaîne puis démarrer le checkout. Aucun préfixe n’est publié si une étape ultérieure bloque. Arrêt avant journal : objets complets non référencés possibles, branche/workspace intacts. Nouvelle chaîne et archive d’origine assurent la rétention après GC.

```text
syrnck native rebase-plan OLD_BASE --onto upstream --text-merge --verify
syrnck native rebase OLD_BASE --onto upstream --text-merge --expect OLD_TIP_ID
```


```cpp
syrnck::NativeRebaseOptions settings;
settings.text_merge.emplace();
auto plan = store.plan_rebase(project, "feature", old_base, new_base, settings, true);
auto result = store.rebase_directory(project, "feature", expected_head,
                                     old_base, new_base, settings);
```


## Préserver le graphe de fusions {#preserve-merges}

`--rebase-merges` rejoue toutes les révisions accessibles depuis HEAD, sauf BASE
et ses ancêtres. BASE doit être ancêtre de HEAD par un lien parent quelconque ;
sinon le plan rend `base_not_ancestor`. La séquence éditable complète place les
parents avant les enfants ; le parcours par défaut traite les branches latérales
avant la branche du premier parent. Chaque fusion garde ses parents remappés dans
leur ordre. Les branches et tags sources restent inchangés.

```text
syrnck native rebase-todo OLD_BASE --onto upstream --rebase-merges > todo.json
syrnck native rebase-plan OLD_BASE --onto upstream --rebase-merges --todo todo.json --verify
syrnck native rebase OLD_BASE --onto upstream --rebase-merges --todo todo.json --expect OLD_TIP_ID
```

Sans `--rebase-cousins`, une branche latérale dont le parent de frontière diffère
de BASE conserve cette frontière, y compris une racine indépendante. Avec
`--rebase-cousins`, ces parents de frontière et racines sont aussi rattachés à
ONTO. Retirer un commit remappe ses enfants vers son premier parent remappé ;
retirer une racine indépendante enlève ce parent à ses enfants. Retirer une fusion
supprime volontairement ses autres liens parents. Si toute l’ascendance
survivante de la source finale est retirée, la cible devient ONTO.

Une fusion est recréée en fusionnant ses parents originaux, en fusionnant ses
nouveaux parents, puis en rejouant les éditions propres au commit de fusion entre
ces deux résultats automatiques. Les éditions manuelles restent conservées sans
réintroduire les changements automatiques d’une branche volontairement retirée.
Les deux calculs utilisent les bases communes récursives bornées, la composition
textuelle facultative et l’alignement des renommages. Un seul budget texte/
renommages couvre toute l’opération, y compris ces calculs intermédiaires.

Une résolution enregistrée de contenu/ajout/suppression n’est réutilisée que si
les conflits d’origine et recréés ont une provenance logique identique. Sinon,
les chemins originaux incertains exigent des choix explicites (`ambiguous_base`),
tout comme les nouveaux conflits entre parents (`merge_recreation`), même si un
fichier provisoire correspond à la source enregistrée. Les ambiguïtés structurelles
restent explicites. Le choix `base` ne peut sélectionner une base originale
provisoire et incertaine.

Utiliser les contextes retournés : pour une fusion, `base` identifie le résultat
automatique des anciens parents, `head` celui des nouveaux parents et `parent`
le premier parent d’origine. Ces identités virtuelles adressées par contenu
peuvent être absentes du Store. Elles lient tous les parents ordonnés ; modifier
une branche latérale invalide aussi les choix ultérieurs.

```cpp
syrnck::NativeRebaseOptions settings;
settings.preserve_merges = true;
settings.rebase_cousins = false;
auto plan = store.plan_rebase(project, "feature", old_base, new_base, settings, true);
```

En plus de `--max-commits`, ce mode accepte `--max-revisions` (défaut 100000 ;
1..1000000) et `--max-links` (défaut 1000000 ; 0..10000000), comptant l’historique
chargé et les candidats virtuels ajoutés, y compris ceux remplacés par regroupement.
`--rebase-max-work` borne le travail comptabilisé de graphe/snapshot/fusion
(défaut 67108864 ; 1..1073741824). La synthèse récursive utilise les mêmes bornes
`--base-max-count/merges/depth/work/input-bytes` que la fusion native.

Le cache virtuel garde des deltas de fichiers par candidat et un snapshot complet
récent. `--rebase-max-delta-bytes` borne l’estimation encodée des deltas (défaut
67108864 ; 1024..268435456). Ce n’est pas une limite mémoire globale : snapshots
courants décodés, métadonnées sources, graphe, buffers texte et rapports ont leurs
coûts propres. Le rapport expose `preserve_merges`, `rebase_cousins`,
`result_merges`, `visited_revisions`, `parent_links`, `graph_work` et les compteurs
`virtual_snapshots`. Les étapes de fusion exposent les deux rapports de parents,
`recorded_resolutions_reused` et `result_parents`.

Le plan reste sans écriture. L’application répète le calcul déterministe vérifié
et matérialise des révisions ordinaires, parents avant enfants ; seule la cible
finale modifie le workspace. L’archive de checkout protège tout le graphe original
et ses blocs après GC. Les règles de reprise signée, chiffrée, packée et de
workspace partiel s’appliquent aussi à ce mode.
