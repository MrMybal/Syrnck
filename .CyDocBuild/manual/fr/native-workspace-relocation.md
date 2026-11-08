---
id: native-workspace-relocation
title: Déplacer un workspace lié
description: Déplacement de dossier récupérable, changements de casse et données locales non versionnées compris.
section: Gestion de versions
order: 224
parent: native-workspaces
sourceRevision: 1
---

## Aperçu et déplacement {#usage}

```text
syrnck native workspace-relocate-plan edit ../project-edit
syrnck native workspace-relocate edit ../project-edit --expect-plan PLAN_ID
syrnck --store .syrnck --workspace edit native head
```

Utiliser le contexte principal et le champ `plan` exact de l’aperçu. La source
doit être un workspace lié inscrit. Le parent cible doit exister sur le même
système de fichiers ; la cible doit être absente. Un alias de casse de la source
reste permis, par exemple `project-edit` vers `Project-Edit`. Une variante
occupée indépendamment est refusée sur un système sensible à la casse.

L’opération déplace le dossier entier : fichiers de travail modifiés ou préparés,
fichiers ignorés ou hors sélection, dossiers vides et liens descendants. Elle ne
parcourt pas les liens, ne reconstruit pas les fichiers depuis HEAD et ne réingère
pas le travail. HEAD logique, index, sélection partielle et branches partagées
restent inchangés. Seuls les dossiers de l’inscription et de HEAD lié changent.
Le dossier principal et le Store restent en place.

L’aperçu lit identités et métadonnées sans écrire d’objets ni changer les fichiers.
Son identité lie toutes les références, les anciennes liaisons, l’identité du
dossier source et celles des deux parents. Elle ne fige pas le contenu des
fichiers : les modifications dans ce même dossier sont déplacées avec lui.
Coordonner éditeurs, observateurs et autres outils pendant l’opération.

## Reprendre ou annuler un déplacement en attente {#recovery}

```text
syrnck native workspace-relocation-status
syrnck native workspace-relocation-recover
syrnck native workspace-relocation-recover --abort
```

Utiliser le contexte principal pour la récupération, même si l’ancien dossier
lié est temporairement absent. Une transaction indique la position physique :
`source`, `bridge` ou `destination`. La reprise atteint la destination ; abort
ramène le même dossier au chemin original avant de conserver les liaisons
originales. Aucune de ces opérations ne supprime récursivement les données.

Le journal est conservé avant tout déplacement. Chaque opération passe par un
dossier frère unique `.syrnck-relocate-NONCE`, changements de casse compris.
Les renommages exclusifs refusent une cible occupée. Inscription et HEAD sont
publiés ensemble après vérification de l’identité physique à destination.
La reprise inspecte les identités réelles sans présumer qu’un renommage est
terminé. Ne pas supprimer ni réutiliser le dossier intermédiaire.

Relancer `workspace-relocate` avec les mêmes nom, destination et plan reprend
le déplacement correspondant. Une opération déjà terminée renvoie son reçu si
les liaisons et l’identité cible correspondent encore. Une liaison modifiée ou
un dossier remplacé exigent une inspection, sans adoption implicite. Sans
transaction active, statut et récupération renvoient
`pending: false, state: none`.

## Sécurité et périmètre {#safety}

La capacité `workspace-relocation-v1` est activée avant le journal ; les anciens
lecteurs refusent le Store. Pendant le déplacement, autres changements de
références, mutations natives, GC et création de sauvegarde sont bloqués jusqu’à
récupération. Remplacement du dossier ou des parents, liens/reparse points à la
racine, identités ambiguës, chevauchements et chemins étrangers occupés sont
refusés. Une édition directe des références ne peut pas contourner le protocole
pour relier une inscription active à un autre dossier.

Les identités utilisent volume/file ID Windows ou device/inode POSIX. Un
fournisseur non pris en charge entraîne un refus prudent. Les derniers callbacks
précèdent une nouvelle vérification d’identité et de références ; la publication
finale n’appelle plus d’observateur. Les écritures Syrnck sont coordonnées, mais
les autres écritures du système de fichiers demandent une coordination. Les
tests d’interruption ne démontrent ni la résistance aux coupures d’alimentation
ni la prise en charge de tous les systèmes réseau.

Ces commandes ne déplacent pas entre systèmes de fichiers, ni le workspace
principal ou le Store. Les reçus terminés restent conservés, sans nettoyage
automatique par âge. Une sauvegarde du Store conserve objets logiques et reçus ;
les octets locaux non préparés nécessitent toujours une
[archive de workspace](doc:native-workspace-archives) ou une sauvegarde des
fichiers. Déplacer un dossier ne chiffre pas ses fichiers de travail en clair.

## API C++ {#api}

```cpp
#include <syrnck/workspace.hpp>
store.use_native_workspace("");
auto plan = syrnck::plan_native_workspace_relocation(store, "edit", destination);
syrnck::relocate_native_workspace(store, "edit", destination,
    plan["plan"].get<std::string>(), operation);
auto pending = syrnck::native_workspace_relocation_status(store);
syrnck::recover_native_workspace_relocation(store, false, operation);
```

Passer `true` pour annuler. Ces opérations sont C++ ; interfaces C et Go gardent
leur ABI actuelle. Progression et annulation peuvent laisser un déplacement en
attente ; son journal conservé est nécessaire pour reprendre ou annuler ensuite.

## Format du journal {#format}

`workspace-relocations/active` conserve un chunk canonique native-workspace-relocation
v1 à quatorze champs : `version`, `type`, `name`, `nonce`, `source`,
`destination`, `bridge`, `identity`, `source_parent`, `destination_parent`,
`registration`, `head`, `refs_hash`, `plan`. Les liaisons sont incluses dans le
descripteur, sans dépendances d’objets cachées. Les racines du contexte restent
en place pendant l’attente. Le validateur central des références n’accepte que
les transitions exactes de début et de fin. La finalisation retire la racine
active et ajoute `archive/workspace-relocations/ID/completed` ou `/aborted`,
conservant le reçu. Versions d’objets, révisions, packs et ABI C inchangées.
