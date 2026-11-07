---
id: native-stage-rebase
title: Rejeu d’un index périmé
description: Guide : Rejeu d’un index périmé.
section: Versionnage
order: 370
parent: native-history
sourceRevision: 1
---

# Rejouer un index préparé {#rebase-a-native-staging-index}

rebase-index-plan rejoue les changements figés de l’index sur la tête actuelle de sa branche en lisant les snapshots/manifestes stockés uniquement. Aucun scan, import ou modification du workspace ; fichiers sales, ignorés et non suivis n’affectent pas le plan. L’index Git est indépendant.

```text
syrnck native staged
syrnck native rebase-index-plan --verify
syrnck native rebase-index-plan --branch feature
```


## Rejeu et aperçu {#replay-and-preview}

Base = parent enregistré de l’index ; current = tête ; incoming = index complet. Les changements indépendants de fichier/mode se combinent. Contenu concurrent, ajout/suppression, casse ou préfixe fichier/dossier bloquent ; texte optionnel, prise en compte des renommages non implémentée. Un journal checkout/switch en attente bloque.

Il faut tête et index. Un index préparé avant premier commit peut ensuite se rejouer sur une base vide ; aucune ascendance imposée à la nouvelle tête. Le CLI suit la branche choisie, --branch ne change pas HEAD.

Index frais : inchangé avec ID/octets exacts. Périmé : candidat v1 daté zéro, marqueur de branche et parent tête actuelle. Un résultat vide reste un index explicite. ID déterministe prédit mais non stocké. Un changement de représentation préparé seul reste si la nouvelle tête garde contenu/manifeste de base, éventuellement avec mode indépendant ; si upstream a aussi changé la représentation à contenu égal, upstream gagne. Un contenu upstream changé ne conserve pas une ancienne représentation de contenu obsolète.

Rapport : head,index,base nullable,stale,changed,target,archive,ready,objects_verified,result_files,empty,counts,changes,conflicts,resolution_context,blockers et resolved. Comparaison du candidat avec la tête, séparant contenu/mode/manifeste. Bloqué : comparaison provisoire, target/result_files null. Prêt 0 ; bloqué 1 avec JSON ; entrée invalide/absente diagnostic.

Plan sans écriture. --verify contrôle les deux fermetures et hashes historiques ; hors texte, métadonnées seules ne prouvent pas les blocs. Texte activé vérifie les fichiers tentés même sans --verify. Snapshots plats, rapport et révision de 16 Mio maximum. Callbacks synchrones non réentrants, progression/arrêt et interface bilingue ; historique C/Go reste à développer.

```cpp
auto plan = store.plan_rebase_stage("feature", true, operation);
```


## Appliquer et conserver les préparations {#apply-and-retain-prepared-bytes}

L’application vérifie le plan prêt avant candidat et exige son ID prédit. Une transaction contrôle tête et index attendus, archive l’ancien sous archive/rebase-index/INDEX_ID puis remplace staging/BRANCH, sans changer branche, HEAD ou workspace. IDs exacts capturés sous verrou au CLI si omis, explicites en C++. Conflits et journal actif bloquent.

Index frais rend unchanged sans écriture mais avec audit complet. Sinon applied, previous_index et nouvel index, plus champs du plan ; changed/stale décrivent l’entrée, staged le nouvel état. Un résultat égal à la tête demeure un index propre explicite, qui ne suffit pas au commit staged exigeant des différences.

Arrêt, bloc absent, callback ou archive contradictoire avant publication gardent les anciennes références ; le candidat peut rester complet non référencé. Aucun journal de fichiers nécessaire. Après engagement, aucun callback/stop ; coupure ancien ou nouvel ensemble complet. Inspecter staged avant nouvel essai.

L’archive protège tout l’ancien index, ses blocs et sa base après GC, même après commit/retrait du remplacement. Pas d’expiration automatique ; format/règles de chiffrement inchangés.

```text
syrnck native rebase-index --expect HEAD_ID --expect-index INDEX_ID
syrnck native staged
syrnck native commit --staged -m "Publish the rebased selection"
syrnck native archives --kind rebase-index --verify
```


```cpp
auto result = store.rebase_stage("feature", expected_head, expected_index, operation);
```


## Résoudre les conflits d’index {#resolve-index-conflicts}

--resolutions prend resolution_context du plan et files complété. operation rebase-index ; head tête actuelle ; source index figé ; base et parent son ancienne base, null pour l’index initial. Tout ID doit correspondre : changement de tête/index impose nouveau plan et choix.

Choix d’entrée entière current (nouvelle tête), incoming (ancienne préparation), base, remove ou file personnalisé avec mode. Seulement chemins initialement conflictuels ; collisions recalculées, résolution incomplète sans publication. Aucun remplacement de chemin propre. Choix vides sur index frais gardent l’ID exact.

Le décodage rejette doublons/champs inconnus/chemins/IDs/choix invalides avec les limites communes de 16 Mio, 100000 chemins et profondeur/champs. Plans par métadonnées contrôlent les manifestes personnalisés ; --verify/application vérifient leurs payloads. Conserver un pin avant publication si maintenance possible ; le nouvel index puis commit conservent ensuite les blocs.

```text
syrnck native rebase-index-plan --resolutions choices.json --verify
syrnck native rebase-index --resolutions choices.json --expect HEAD_ID --expect-index INDEX_ID
```


```cpp
auto choices = syrnck::NativeResolutionSet::parse(plan["resolution_context"]);
choices.files["Content/Map.uasset"] = {syrnck::NativeResolutionChoice::incoming};
auto preview = store.plan_rebase_stage_resolved("feature", choices, true, operation);
auto result = store.rebase_stage_resolved("feature", expected_head, expected_index,
                                          choices, operation);
```


## Composition textuelle optionnelle {#optional-bounded-text-composition}

--text-merge combine les changements UTF-8 indépendants entre base enregistrée, tête et index figé, sans lire le travail ultérieur. Résolutions explicites prioritaires ; binaire, chevauchements, ajouts/suppressions et namespace bloquent l’ensemble. Fins de ligne conservées et mode combiné indépendamment.

Même lot de 64 tentatives et 16 Mio entrées/résultats propres, plus limites par fichier. Les options de budget exigent l’activation. text_merge décrit statuts/motifs/coordonnées/métadonnées par tentative, sans texte fusionné. Index déjà frais : ID exact et zéro tentative.

Le plan reconstruit/vérifie seulement les conflits éligibles et prédit les manifestes CDC sans écrire. L’application audite les entrées avant matérialisation et candidat ; ancienne archive et nouvel index publient dans le même CAS tête/index. Aucun groupe partiel en conflit. Avant transaction, arrêt peut laisser des objets complets mais garde références/workspace. Archive/base restent reconstructibles après GC et le commit ultérieur conserve le snapshot fusionné. Les méthodes nommées with_options gardent les entrées plain/resolved compatibles.

```text
syrnck native rebase-index-plan --text-merge --verify
syrnck native rebase-index --text-merge --expect HEAD_ID --expect-index INDEX_ID
```


```cpp
syrnck::NativeStageRebaseOptions settings;
settings.text_merge.emplace();
auto plan = store.plan_rebase_stage_with_options("feature", settings, true);
auto result = store.rebase_stage_with_options("feature", expected_head,
                                               expected_index, settings);
```
