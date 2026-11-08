---
id: native-workspaces
title: Workspaces natifs liés
description: Dossiers de travail, HEAD et index indépendants sur un Store partagé.
section: Versionnage
order: 222
parent: native-branches
sourceRevision: 3
---

## Créer et sélectionner un workspace {#usage}

```text
syrnck native workspace-add edit ../project-edit --branch feature
syrnck native workspaces
syrnck --store .syrnck --workspace edit native head
syrnck --store .syrnck --workspace edit native status --combined
syrnck --store .syrnck --workspace edit native add Source
syrnck --store .syrnck --workspace edit native commit --staged -m "Feature work"
```

Le workspace principal conserve son organisation historique. Un workspace lié
enregistré possède son répertoire canonique, HEAD, index préparé, révision
détachée, reçus de checkout, état bisect, cache de travail et journal d’observation.
Objets et branches nommées restent partagés ; les données ne sont pas copiées
entre plusieurs Stores.

`workspace-add NAME DIRECTORY` s’exécute dans le contexte principal. Le dossier
parent doit exister. Une destination absente est créée ; les fichiers existants
sont conservés et les règles habituelles de collision s’appliquent. `--branch NAME`
exige une branche existante et disponible. `--revision REVISION` initialise HEAD
détaché ; sans ces options, la révision principale committée est utilisée en mode
détaché. Un principal non né initialise un snapshot vide retenu.

Les noms contiennent 1 à 64 lettres ASCII minuscules, chiffres ou tirets internes.
`primary` et les noms de périphériques Windows sont réservés. Les racines ne
peuvent chevaucher ni un autre workspace, ni le principal, ni le Store. Liens et
alias portables ne contournent pas ces contrôles. Maximum : 1 024 contextes liés.

`--workspace NAME` sélectionne le contexte de l’invocation ; `primary` désigne
explicitement le défaut. Utiliser `--store` pour retrouver le Store partagé depuis
un autre dossier. En contexte lié, les commandes natives prennent le répertoire
enregistré comme `--directory` par défaut ; une valeur explicite doit résoudre
cette même racine. `native workspaces` affiche aussi le principal, les répertoires,
branches/révisions et checkouts en attente.

## Isolation et réservations de branches {#isolation}

La préparation d’un workspace ne consomme jamais l’index d’un autre. Commits et
réécritures utilisent la cible sélectionnée. Une branche nommée ne peut être
sélectionnée que dans un workspace ; les plans signalent `branch_in_other_workspace`.
La publication des références contrôle aussi les réservations : une branche
explicite ne permet pas de déplacer celle d’un autre workspace derrière ses
fichiers. Changer sa sélection ou détacher HEAD pour libérer cette branche.

Chaque handle Store garde son verrou exclusif de dépôt. Les processus l’ouvrent
autour des opérations et les observateurs le libèrent pendant l’attente. Un
checkout en attente bloque mutations natives et GC du Store jusqu’à sa récupération
dans le contexte propriétaire. L’application est journalisée par workspace, sans
atomicité globale entre les dossiers de travail.

## Observation {#observation}

```text
syrnck --store .syrnck --workspace edit native watch --combined --journal
```

L’observation liée utilise les notifications natives disponibles et contrôle
l’identité des références et de la configuration du Store externe. Une publication
de métadonnées impose une observation complète sans émettre de chemin extérieur
au workspace. Stamps non pris en charge et échecs de notifications replient vers
le polling. Les audits complets périodiques restent nécessaires. Cache et journal
de chaque contexte sont séparés.

## Mesure de l’index de travail {#performance}

```text
python scripts/benchmark_working_index.py --cli build/Release/syrnck.exe --linked --asset-mib 64 --iterations 5 --output linked-index.json
```

Omettre `--linked` pour le principal ; sous Unix utiliser `build/syrnck`.
Le jeu de données synthétique de 64 Mio vérifie l’équivalence des statuts complet/avec
index et détecte une modification de même taille avec date de modification
restaurée. Sous Windows x64 local, le 8 novembre 2026, les cinq observations
liées à chaud réutilisent chacune un fichier et hashent zéro octet ; les
observations complètes hashent chacune 64 Mio. Médianes CLI : 68 ms complètes,
24 ms avec index (principal : 75 ms et 20 ms). Le cache OS était chaud et d’autres
tests tournaient. Ces mesures montrent les lectures évitées sur ce cas ; elles
ne prédisent ni le débit d’un projet Unreal ni les latences multiplateformes.

## Sauvegarde et restauration {#restore}

```text
syrnck --store .syrnck backup create /backup/snapshot
syrnck --store /backup/snapshot backup restore /recovered/project/.syrnck
syrnck --store /recovered/project/.syrnck native workspace-restore edit /recovered/edit --directory /recovered/project
```

Les sauvegardes omettent les liaisons actives aux dossiers et les reçus locaux.
Elles conservent index, données détachées et racines de cibles terminées, avec un
descripteur de sélection portable et une racine de révision exacte par workspace
lié. Le format v2 enregistre ces exclusions pour les sources expérimentales sans
capacités requises. Un Store avec capacités de lecteur requises utilise le
marqueur v3, qui les lie aussi à la sauvegarde. Les v1/v2 restent lisibles.

`workspace-restore NAME DIRECTORY` lie le snapshot portable à un nouveau dossier.
La branche d’origine est réutilisée si sa tête est inchangée et non réservée.
`--branch NEW_NAME` peut créer une branche de sauvetage ; `--detach` restaure un
HEAD indépendant. Une autre tête existante n’est jamais réinitialisée implicitement.
L’index figé est matérialisé en fichiers de travail, avec HEAD committé conservé
séparément. Les modifications non préparées ne font pas partie d’une sauvegarde
logique du Store. Changer la cible de l’index archive son snapshot d’origine avant
de publier le remplacement.

La restauration publie exclusivement le dossier exporté complet avant de lier
le contexte. Après interruption avant liaison, relancer : une arborescence
existante doit correspondre intégralement au contenu et aux modes sauvegardés.
Exports modifiés, fichiers supplémentaires, liens et métadonnées protégées sont
refusés sans écrasement. Les racines portables restent conservées jusqu’à un
retrait explicite de rétention.

## Récupération {#recovery}

```text
syrnck native workspaces
syrnck --store .syrnck --workspace edit native checkout-status
syrnck --store .syrnck --workspace edit native checkout-recover
syrnck --store .syrnck --workspace edit native checkout-recover --abort
```

L’enregistrement retient atomiquement un état détaché vide valide avant la première
matérialisation. Si celle-ci bloque ou s’interrompt, le contexte reste consultable ;
y effectuer switch, checkout ou récupération. HEAD et préparations du principal
ne sont pas déplacés. Les reçus lient contexte et répertoire canonique exact ;
un autre contexte ne peut les exécuter. Les reçus terminés restent locaux et
consultables après changement de sélection et GC.

La campagne Windows x64 a vérifié les 808 points IO observés d’arrêt forcé pour
l’enregistrement et l’entrée/sortie/déplacement du checkout lié : Stores en clair
et chiffrés, reprise/annulation, transitions de namespace, inspection indépendante
de production et GC après récupération. Ces essais gardent OS et système de
fichiers actifs ; les coupures de courant demandent une validation distincte.

## C++, C et Go {#api}

```cpp
store.register_native_workspace(primary, "edit", directory, operation);
store.use_native_workspace("edit");
store.switch_directory(directory, "feature", operation);
auto changes = store.workspace_status(directory, store.native_target(), operation);
store.use_native_workspace(""); // principal, sélection locale au handle
```

En C : `syrnck_store_use_workspace` et `syrnck_workspace_open_registered_ex`.
Une session liée exige un Store sélectionné sur le même nom. Un échec de sélection
conserve le nom précédent. Go expose `Store.UseWorkspace(name)` et
`WorkspaceOptions{Name: "edit"}` ; annulation et sérialisation des handles restent
applicables. Un nom vide sélectionne le principal. Ces symboles s’ajoutent à
l’ABI 1 et demandent une bibliothèque correspondante.

## Formats et périmètre actuel {#formats}

Les références principales et reçus v1 à v7 restent inchangés. Les métadonnées
liées utilisent de vraies références `workspaces/NAME/`. Le slot d’un index nommé
utilise le digest BLAKE3 du nom de branche, pour préserver la longueur maximale
existante ; le snapshot garde le nom complet. Les reçus liés passent en v8 avec
`context` et `layout_version` de 1 à 6. Versions d’objets/révisions, packs et ABI C
inchangées.

Matérialisation partielle, déplacement en place et désenregistrement archivé
restent à compléter. Ce mode matérialise actuellement des snapshots complets
et exige leurs objets retenus complets localement.

## Compatibilité des lecteurs {#compatibility}

Enregistrement et restauration portable établissent l’exigence de lecteur
`linked-workspaces-v1` avant publication de la liaison. Les anciens exécutables
refusent le Store, même pour les commandes de références ordinaires, et ne
peuvent donc contourner les réservations de branches. Les lecteurs à jour
acceptent toujours les configurations classiques. Voir [inspection et mise à
niveau explicite](doc:settings#reader-features). Cette protection n’active pas la
matérialisation partielle, qui reste un travail distinct.
