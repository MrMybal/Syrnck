---
id: native-merge
title: Fusion de branches
description: Guide : Fusion de branches.
section: Versionnage
order: 300
parent: native-history
sourceRevision: 4
---

# Fusion native {#native-merge}

merge-plan calcule topologie et snapshot complet proposé sans modifier objets, références ou workspace. Les décisions de fichiers à trois voies sont partagées avec le replay. Branche sélectionnée et dossier courant par défaut ; options branche/dossier pour les remplacer. Le CLI résout les sélecteurs, C++ utilise IDs exacts. Budgets de révisions/liens identiques à l’[ascendance](doc:native-history). Le graphe complet est nécessaire, sans repli shallow.

```text
syrnck native merge-plan feature --verify
syrnck native merge-plan feature --base BASE_ID
syrnck native merge-plan imported --allow-unrelated-histories
```


## Topologie {#topology}

up_to_date : source ancêtre de cible, égalité comprise, snapshot actuel exact. fast_forward : cible ancêtre de source, snapshot source exact avec sa représentation. merge : divergence, fusion à trois voies sur la base choisie.

Une meilleure base unique est automatique. Plusieurs donnent multiple_merge_bases ; --base doit choisir une des meilleures annoncées, pas un ancêtre plus ancien. Aucune base récursive virtuelle de Git n’est synthétisée. Sans ancêtre, unrelated_histories bloque sauf --allow-unrelated-histories, qui choisit l’arbre vide ; pas de base explicite alors.

Les éditions indépendantes et changements contenu/mode indépendants d’un même fichier se combinent. Contenus concurrents différents, add/add et suppression/modification restent en conflit. Les collisions portables de casse/préfixe sont recalculées. L’égalité logique utilise hash complet/taille/mode, pas le seul découpage ; fast-forward adopte néanmoins la révision exacte.

Sans conflit de snapshot, le plan de checkout exige suivis propres et contrôle ignorés/non-suivis, protections, liens, index et récupération. Une divergence de topologie seule peut être prête sans changement d’octets ; up-to-date reste soumis à ce contrôle prudent. --resolutions accepte les [choix explicites](doc:native-resolutions). Composition textuelle optionnelle ci-dessous ; fusion tenant compte des renommages et synthèse récursive restent à développer.

## Composition textuelle explicite {#opt-in-text-composition}

NativeMergeOptions::text_merge active les limites TextDiffOptions ; vide, les décisions de fichiers et l’inspection de métadonnées restent inchangées. Les résolutions explicites passent d’abord ; seuls les conflits content restants sont tentés. Add/add, suppression/modification, binaire, éditions chevauchantes et namespace restent bloquants ; aucune modification de topologie ou représentation fast-forward.

Chaque tentative reconstruit/vérifie les trois entrées même sans audit historique. Limites de taille et de lot avant lecture des payloads. Au plus 64 conflits, budget agrégé de 16 Mio pour entrées vérifiées et résultats propres, plus plafonds par fichier de travail/lignes/trace/sortie. Un refus conserve le conflit initial avec son motif, jamais de résultat partiel pour tenir un budget.

CLI plan/application : --text-merge ; max-file-bytes, max-lines, max-work, max-output-bytes exigent cette option. Garder les mêmes limites aux deux étapes ; l’application recalcule avec la tête attendue. Progression/diagnostics bilingues, JSON canonique.

text_merge rend limites, compteurs, manifestes générés et statuts/plages/métadonnées par chemin, null si désactivé, sans texte fusionné. Les actions réfèrent des IDs prédits qui peuvent ne pas exister ; aucun objet n’est créé au plan, même si un fichier se compose et un autre bloque. Voir la [préparation virtuelle](doc:text-merge#virtual-file-preparation).

L’application recalcule, vérifie histoires et workspace avant toute matérialisation. Chunks/manifestes doivent correspondre aux IDs prédits avant création de la révision à deux parents. Le checkout récupérable publie l’ensemble. Un échec avant journal peut laisser des objets générés non référencés sans changer branche/workspace ; ensuite resume/abort utilise le candidat retenu sans refaire la fusion. Annulation/observateurs respectent ces frontières.

```cpp
syrnck::NativeMergeOptions settings;
settings.text_merge.emplace(); // Default text limits, explicit opt-in.
auto preview = store.plan_merge(project, "main", source_id, settings, true, operation);
auto result = store.merge_directory(project, "main", expected_tip, source_id,
                                    "Merge feature", settings, operation);
```


```text
syrnck native merge-plan feature --text-merge --verify
syrnck native merge feature --text-merge --expect OLD_TIP_ID
syrnck native merge-plan feature --text-merge --max-file-bytes 2097152 --max-work 33554432
```


## Conserver les contenus concurrents {#conflict-copies}

```text
syrnck native merge-plan feature --text-merge --conflict-copies --verify
syrnck native merge feature --text-merge --conflict-copies --expect OLD_TIP_ID
```

`--conflict-copies` active explicitement une politique de conservation par fichier.
Les choix `--resolutions` s’appliquent d’abord, puis la composition textuelle
bornée si activée. Pour les conflits de contenu ou ajout/ajout restants, une
version conserve le chemin initial et l’autre devient une copie voisine. Le
plus petit ID complet de révision dans l’ordre lexicographique garde le chemin
initial : inverser les deux pairs produit le même ensemble de fichiers. Cet
ordre déterministe ne désigne pas la version la plus récente ou la meilleure.
Une suppression concurrente à une modification conserve le fichier modifié au
chemin initial et annonce `choice: keep_modified`. Les deux historiques parents
conservent la suppression. Deux suppressions identiques suppriment le fichier.

Le nom de copie combine au plus 64 octets UTF-8 du radical, `.syrnck-conflict-`,
l’ID complet de la révision perdante et 16 chiffres hexadécimaux du hash du chemin
initial. Les extensions d’au plus 16 octets UTF-8, dont `.uasset` et `.umap`, sont
conservées. Date, machine et hasard n’influencent pas le résultat. Les rapports
incluent `conflict_copies` et les lignes `resolved` avec `choice: conflict_copy`,
`copy_path`, `copy_revision`, le `reason` initial et les deux descripteurs.
Les copies sont des fichiers suivis ordinaires : les examiner puis les supprimer
dans un commit ultérieur après avoir choisi la version souhaitée de l’asset.

Un nom généré ou parent occupé dans les snapshots d’entrée bloque avec
`conflict_copy_collision`, y compris alias de casse, dossier et fichier qui
aurait autrement été supprimé. Les obstacles de travail ignorés ou non suivis
bloquent toujours le checkout. Conflits de casse, fichier/dossier, bases ambiguës
et historiques sans lien conservent leurs politiques habituelles. Cette option
ne déduit pas un renommage édité et ne compose pas les données binaires.
Les copies réutilisent manifests et blocs existants sans payload binaire
supplémentaire dans le stockage. L’application vérifie les historiques complets
et utilise signature et checkout récupérable habituels. Le candidat figé, noms
de copies compris, reprend sans recalculer la politique ni exiger la clé de signature.

En C++, définir `NativeMergeOptions::conflict_copies = true`, faux par défaut.
Cherry-pick, revert et rebase gardent leurs politiques de résolution existantes.

## Rapports et vérification {#reports-and-validation}

Champs : mode, head, source, base choisie, merge_bases, compteurs de graphe, ready, objects_verified, result_files, changes, conflicts, resolved, resolution_context, blockers et workspace. Blocage de topologie : fichiers/workspace null ; conflits fichiers : workspace null ; sinon target du workspace null car aucune révision candidate n’est écrite. Code 0 prêt, 1 bloqué avec JSON, diagnostic pour entrée/métadonnée invalide.

--verify audite entièrement les deux histoires et fichiers, résolutions personnalisées comprises. Sans lui, métadonnées et workspace sont contrôlés mais pas les blocs entrants, sauf tentative textuelle ciblée. Rapport/candidat bornés comme le replay ; mémoire selon graphe, snapshots et résultat. Callbacks non réentrants, arrêt coopératif et interface bilingue. Merge natif non exposé en C/Go.

```cpp
syrnck::NativeMergeOptions settings;
settings.base = selected_best_base; // Optional explicit best ancestor.
auto plan = store.plan_merge(project, "main", source_id, settings, true, operation);
```


## Appliquer et récupérer {#apply-and-recover}

L’application recalcule toujours un plan entièrement vérifié et exige la tête exacte attendue, capturée sous verrou par le CLI si --expect est omis. Message par défaut Merge SOURCE_ID.

Up-to-date rend state unchanged sans écriture. Fast-forward restaure la révision source exacte sans commit supplémentaire. Divergence : révision aux parents [ancienne cible, source], message et date UTC courante, même avec arbre inchangé. Une [identité configurée](doc:native-authorship) crée une v2 avec auteur/committer actuels et signature facultative ; sinon elle reste v1. Branches/tags source et HEAD symbolique ne bougent pas.

--no-ff exige deux parents même en avance rapide ; --ff-only refuse la divergence avec fast_forward_required. Options exclusives, valides au plan/application, sans contourner base, histoires sans lien, conflits ou workspace.

Le candidat passe par journal, sauvegardes de fichiers et publication atomique branche/archive/reçu. Abort avant publication seulement ; après, reprise du nettoyage. Un journal actif bloque GC et nouvelles mutations natives ; ouvrir le Store ne récupère jamais automatiquement le workspace. Des métadonnées préparées avant échec peuvent rester sans référence. Les fichiers privés restent en clair, y compris en Store chiffré.

Les deux histoires sont accessibles via le commit à deux parents ; fast-forward inclut déjà l’ancienne cible et les archives checkout retiennent aussi les têtes. Les blocs restent protégés après GC. Revert de fusion utilise --mainline N.

La réponse directe ajoute operation merge, mode, previous_revision, merged_revision, base aux champs de checkout. La récupération rend le journal générique et sa cible, sans changer les formats révision/journal.

```text
syrnck native merge feature --expect OLD_TIP_ID -m "Merge feature"
syrnck native merge release --ff-only
syrnck native merge feature --no-ff -m "Record feature integration"
```


```text
syrnck native checkout-status
syrnck native checkout-recover
syrnck native checkout-recover --abort
```


```cpp
auto applied = store.merge_directory(project, "main", expected_id, source_id,
                                      "Merge feature", settings, operation);
```

## Plans tenant compte des renommages {#renames}

`--rename-similarity 50` active l'[alignement](doc:native-file-history#rename-merges).
Répéter les options pour planifier et appliquer. Les choix structurels restent
explicites ; `--text-merge` compose le texte compatible après alignement.
