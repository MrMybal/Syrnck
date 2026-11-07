---
id: native-hard-reset
title: Reset hard récupérable
description: Guide : Reset hard récupérable.
section: Versionnage
order: 260
parent: native-history
sourceRevision: 1
---

# Reset hard natif récupérable {#recoverable-native-hard-reset}

Le reset hard remplace les fichiers suivis du workspace de la branche sélectionnée et vide son index figé. Avant remplacement, il capture les octets locaux complets et conserve ancienne tête et index dans un snapshot de récupération.

--directory choisit le workspace avec Store strictement intérieur ; --branch doit être sa branche sélectionnée. Les contrôles de liaison restent appliqués. Toute récupération checkout/switch/reset doit être terminée/annulée d’abord. Soft/mixed et reset d’index gardent leurs contrats distincts.

```text
syrnck native reset-plan TARGET --hard --verify
syrnck native reset TARGET --hard --expect OLD_HEAD --expect-plan PLAN_ID
syrnck native reset HEAD --hard
```


## Données remplacées et protégées {#what-is-replaced-and-protected}

Le plan en lecture seule inventorie le namespace et hashe les fichiers sélectionnés, sans se fier aux ignores, tailles ou dates. Il inclut chemins présents suivis par tête/index, fichiers ordinaires non suivis faisant obstacle à une cible/transition/orthographe, et dossiers nécessaires à retirer/recréer ces transitions, vides compris.

Un suivi absent reste absent dans la capture puis revient s’il existe dans la cible. Modifiés, ajouts/suppressions préparés et index périmés peuvent être réinitialisés. Casse et fichier/dossier utilisent le journal d’évacuation. Non-suivis sans rapport restent sur place. obstructing_untracked indique ceux retirés du workspace mais reconstructibles dans le snapshot de récupération.

Liens/reparse, fichiers spéciaux, noms portables ambigus et entrées Git/Store protégées ne sont pas adoptés : blocage avant candidat. Retrait de dossier non récursif, vide après évacuation des fichiers capturés.

Le plan lie racines canoniques workspace/Store, HEAD sélectionné, tête/index d’origine, cible, orthographes/hashes/modes exacts et profil des dossiers source. Rétablir taille/date n’évite pas ce contrôle. --verify ajoute l’audit complet des fermetures sans changer l’ID. C++ exige cet ID ; le CLI peut capturer un plan frais immédiatement avant application si --expect-plan est omis.

## Application et récupération {#application-and-recovery}

Avant import des données de travail, vérifier anciennes versions conservées et cible. Chaque fichier ingéré doit encore correspondre au hash complet/taille/mode du plan. La révision de récupération contient ses fichiers capturés et comme parents ancienne tête/index éventuel, préservant travail sale et préparation indépendante après GC.

Le journal checkout v5 distingue source de travail et tête, index original, sélection HEAD et profil de dossiers. Publication initiale protège source/cible avant déplacement ; publication finale déplace branche, retire index et crée archive ensemble.

Resume termine installation/nettoyage. Abort avant publication finale restaure octets antérieurs, absences, orthographes et dossiers vides capturés, avec tête/index inchangés. Après publication, nettoyage seulement. Ouvrir le Store ne récupère rien implicitement. Mutations/obstructions nouvelles sont refusées en gardant les données observées pour récupération explicite.

Succès : operation reset, mode hard, ancienne révision/index, recovery_revision, archive, plan et reçu checkout. L’archive cible la récupération ; ses parents gardent tout l’historique et l’index indépendamment de la branche. Exporter récupération pour travail, index pour préparations, ou inspecter les archives checkout.

L’export restaure des fichiers ; les dossiers vides ne sont pas suivis dans la révision plate, seulement le journal pour abort. Les archives restent jusqu’au retrait avec sauvegarde vérifiée. Même tête propre sans index : unchanged sans écriture. Erreur de préparation peut laisser des objets valides non référencés ; journal actif à récupérer avant GC. Préparations/backups en clair comme le checkout ordinaire.

```text
syrnck native checkout-status
syrnck native checkout-recover
syrnck native checkout-recover --abort
```


```text
syrnck native export RECOVERY_REVISION saved-working-files
syrnck native export PREVIOUS_INDEX saved-staged-files
syrnck native archives --kind checkout --verify
```


## API et bornes {#api-bounds-and-validation}

Les fonctions de syrnck/workspace.hpp fonctionnent sans réseau. Métadonnées plates, rapports, profil et inventaire gardent chacun 16 Mio. Au plus 100000 entrées de dossiers source, profondeur 128. Les fichiers passent dans le pipeline d’ingestion configuré en flux, avec inventaire/hash des fichiers choisis. Cela ne repose pas sur le cache consultatif ou le watcher.

```cpp
auto plan = syrnck::plan_native_hard_reset(store, project, "main", target, true);
auto applied = syrnck::native_hard_reset(store, project, "main", expected_head,
                                         target, plan["plan"].get<std::string>());
```
