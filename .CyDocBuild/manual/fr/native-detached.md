---
id: native-detached
title: HEAD natif détaché
description: Explorer des révisions et committer indépendamment des branches nommées.
section: Versionnage
order: 221
parent: native-branches
sourceRevision: 2
---

## Sélectionner une révision détachée {#usage}

```text
syrnck native switch-plan --detach tags/release --verify
syrnck native switch --detach tags/release
syrnck native head
syrnck native commit -m "Detached experiment"
syrnck native branch rescue HEAD
syrnck native switch main
```

`switch --detach REVISION` restaure un snapshot committé sans sélectionner ni
déplacer une branche nommée. Les sélecteurs de révision sont ceux du checkout.
`native head` indique `branch: null`, `mode: "detached"`,
`reference: "workspace/detached"` et la révision committée. Aucune fausse branche
n’apparaît dans `native branches`. `detach-history` est une autre opération :
elle crée une nouvelle racine d’ascendance pour la cible sélectionnée.

Par défaut, commits natifs, préparation, statut, historique, réécritures, stash et
observation du workspace utilisent la révision détachée et son index indépendant.
Les commits prennent la révision détachée précédente comme parent. Une option
explicite `--branch NAME` opère toujours sur cette branche sans la sélectionner.
La publication native distante exige une branche nommée ; créer une branche de
sauvetage avant de publier.

`native checkout REVISION` déplace la cible sélectionnée : branche en mode
attaché, révision privée en mode détaché. `native switch NAME` revient sur une
branche et restaure ses fichiers ; aucune tête de branche ne bouge lors du switch.

## Préflight et récupération {#recovery}

Les plans sont en lecture seule. Fichiers suivis modifiés, index sur l’une des
cibles, collisions non suivies et namespaces dangereux bloquent le détachement
ou le retour à une branche selon les règles ordinaires du checkout. Aucun
écrasement ou reset d’index implicite. Les transitions fichier/dossier utilisent
le même exécuteur récupérable de namespace.

Avant de détacher un workspace attaché, la destination est vérifiée et retenue
dans un slot privé. Remplacer une ligne détachée inactive archive son ancienne
tête sous `archive/OLD_ID`. Une interruption durant cette préparation laisse
HEAD et les fichiers d’origine intacts, avec un candidat retenu supplémentaire.
Dès que le journal de checkout existe, employer `native checkout-status` puis
`native checkout-recover` (ou `--abort`). HEAD change avec le journal terminé.

## Rétention et formats {#retention}

La racine de révision privée `workspace/detached` et l’index optionnel
`workspace/staging` retiennent toutes leurs dépendances pendant le GC. Revenir
sur une branche conserve la tête détachée. Archives, branches de sauvetage et
tags protègent les anciennes lignes ; les sauvegardes portables conservent ces
racines de données privées en omettant le HEAD lié à la machine. Les règles de
retrait explicite habituelles restent applicables aux archives.

Les descripteurs HEAD nommés gardent leur version canonique 1. Les descripteurs
détachés utilisent la version 2 avec `branch: null` et les mêmes champs directory/type.
Les reçus de checkout impliquant une cible détachée utilisent la version 7 et
`layout_version` pour le layout correspondant, de 1 à 6. Les reçus entre branches
gardent leurs formats. Les stash détachés passent en version 2, ceux des branches
restent en version 1. Les anciens exécuteurs doivent refuser les nouveaux reçus.
Formats d’objets/révisions, formats physiques des packs et version ABI C inchangés.

## Intégration {#api}

`Store::native_target()` retourne une branche nommée ou le sélecteur opaque `:HEAD`.
Le passer aux API natives prenant une branche/cible opérationnelle. Les API de
snapshots prennent toujours des IDs : appeler `resolve_native_revision("HEAD")`.
`:HEAD` ne peut pas être un nom de branche. Les anciens champs de plan/statut
nommés `branch` contiennent ce sélecteur en mode détaché, tandis que
`native_head().branch` vaut null. Les API workspace C/Go suivent aussi HEAD détaché
lorsque le sélecteur est omis.

```cpp
auto plan = store.plan_detached_checkout(workspace, revision, true, operation);
if (plan["ready"].get<bool>())
  store.detach_checkout(workspace, revision, operation);
auto target = store.native_target();
auto changes = store.workspace_status(workspace, target, operation);
```

Chaque handle Store sélectionne un contexte. Les [workspaces liés](doc:native-workspaces)
partagent objets et branches avec des HEAD et index indépendants. Matérialisation
partielle, déplacement en place et désenregistrement archivé restent à réaliser.
