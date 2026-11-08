---
id: native-workspace-archives
title: Archiver et restaurer un workspace
description: Désenregistrer un workspace lié en conservant les fichiers modifiés, la préparation et l’historique.
section: Gestion de versions
order: 223
parent: native-workspaces
sourceRevision: 2
---

## Désenregistrer un workspace lié {#unregister}

```text
syrnck native workspace-unregister-plan edit --include-untracked --verify
syrnck native workspace-unregister edit --include-untracked --expect-plan PLAN_ID
syrnck native workspace-archives --verify
syrnck native workspace-archive ARCHIVE_ID --verify
```

Reprendre le champ `plan` de l’aperçu, avec le même choix pour
`--include-untracked`. Ces commandes sélectionnent elles-mêmes le contexte lié
nommé. Le workspace principal ne peut pas être désenregistré et les checkouts
en attente doivent d’abord être récupérés. L’aperçu lit les fichiers et références
sans publier d’objets ni de métadonnées. Une modification du contenu capturé,
de la sélection ou des références du contexte invalide le plan.

L’application capture l’état physique et conserve une archive canonique avant
de retirer l’inscription et toutes les références `workspaces/NOM/` dans une seule
transaction. Le dossier original et tous ses fichiers restent en place ; la
réservation de branche est libérée. Les branches partagées et HEAD principal
restent inchangés. Le handle C++ revient au contexte principal.

## Contenu conservé {#contents}

L’archive conserve trois états indépendants :

- HEAD commité complet, y compris les fichiers hors sélection partielle.
- L’index figé complet, s’il existe, avec son parent original même s’il est périmé.
- Une révision de récupération physique contenant les fichiers suivis sélectionnés,
  leurs modifications locales et les fichiers ordinaires faisant obstacle à
  l’arbre logique. Les suppressions restent absentes de ce snapshot physique.
  `--include-untracked` ajoute les fichiers ordinaires sélectionnés, non ignorés,
  trouvés par le parcours natif.

Chaque référence originale du contexte possède une racine d’archive typée
explicite, notamment les index inactifs et références de récupération ou
d’historique terminées. GC, packs et [sauvegardes](doc:backups) conservent leurs
fermetures d’objets. Les fichiers locaux ignorés ou hors sélection restent dans
le dossier original ; leur présence dans le snapshot n’est pas garantie. Les
dossiers vides ne sont pas versionnés. Des chemins dangereux, liens ou métadonnées
protégées peuvent bloquer le plan ; inspecter `ready` et les obstacles signalés.

L’inventaire accepte `--after ID`, `--limit N` (1..1000, 100 par défaut) et
`--verify`. Les identifiants stables sont ceux des objets d’archive ; la pagination
les trie par identifiant. `workspace-archive` affiche la recette des références
sauvegardées et la révision physique.

## Restaurer dans un nouveau dossier {#restore}

```text
syrnck native workspace-archive-restore-plan ARCHIVE_ID recovered ../recovered --detach --verify
syrnck native workspace-archive-restore ARCHIVE_ID recovered ../recovered --detach --expect-plan PLAN_ID
syrnck --store .syrnck --workspace recovered native status --combined
```

Exécuter la restauration depuis le contexte principal. Le parent du dossier
cible doit exister ; la cible ne doit chevaucher ni le principal, ni le Store,
ni un autre workspace inscrit. Un dossier existant n’est réutilisé que si ses
fichiers ordinaires et modes correspondent à tout le snapshot physique. Les
fichiers ajoutés ou modifiés et les liens sont refusés. Des dossiers vides
supplémentaires restent permis dans un export exact.

La sélection originale, nommée ou détachée, est utilisée par défaut.
`--branch NOUVEAU_NOM` restaure sous une autre branche ; `--detach` crée un HEAD
privé. Une branche nommée doit être absente ou encore pointer sur HEAD sauvegardé,
sans réservation concurrente. Une branche avancée n’est jamais réinitialisée.
HEAD garde la révision commitée, le dossier contient les octets modifiés capturés
et l’index préparé reste distinct. Changer son sélecteur réécrit uniquement le
message de l’index, sans rebaser implicitement un index périmé.

L’export physique est publié avant l’inscription. Après interruption, relancer
la même commande avec le même plan : un export terminé exact est réutilisé et
une inscription terminée exacte est idempotente. Les fichiers ou liaisons
modifiés sont refusés. La sélection partielle est restaurée, mais les fichiers
ancêtres faisant obstacle sont restitués intégralement. Les anciennes références
inactives restent accessibles dans l’archive sans être réactivées sous le nouveau
nom. Le nouveau contexte conserve aussi sa propre racine de récupération physique.

## Sauvegardes et retrait des archives {#retention}

```text
syrnck backup create /existing-backup-parent/with-workspace-archive
syrnck backup retire-plan archive/workspaces/ARCHIVE_ID --from /existing-backup-parent/with-workspace-archive
syrnck backup retire archive/workspaces/ARCHIVE_ID --from /existing-backup-parent/with-workspace-archive --expect-plan RETIRE_PLAN
```

Le retrait exige une sauvegarde vérifiée contenant le groupe exact complet.
Un membre `/record`, `/head`, `/work` ou `/roots/...` ne peut pas être retiré seul.
Le retrait supprime des racines, sans effacer les fichiers de travail ni lancer
le GC. Un contexte restauré garde sa racine de récupération physique après le
retrait de l’archive source. HEAD et l’index sélectionnés restent des racines
ordinaires ; les anciennes références inactives dépendent ensuite de la
sauvegarde vérifiée ou d’autres racines restantes. Voir le
[retrait des archives](doc:archive-retention).

Pour récupérer après GC local, restaurer cette sauvegarde dans un Store séparé,
puis y exécuter `workspace-archive-restore-plan` et `workspace-archive-restore`
en indiquant le dossier principal récupéré avec `--directory`. Les liaisons
absolues historiques sont des recettes, sans inscription active ; le dossier
original n’a pas besoin d’exister.

## API C++ et limites actuelles {#api}

```cpp
#include <syrnck/workspace.hpp>
store.use_native_workspace("edit");
auto plan = syrnck::plan_native_workspace_unregistration(store, true, true);
auto archived = syrnck::unregister_native_workspace(
    store, plan["plan"].get<std::string>(), true);
auto id = archived["archive"].get<std::string>();
syrnck::NativeWorkspaceRestoreOptions settings;
settings.detached = true;
auto restore = syrnck::plan_native_workspace_archive_restore(
    store, primary, id, "recovered", destination, settings, true);
syrnck::restore_native_workspace_archive(store, primary, id, "recovered",
    destination, restore["plan"].get<std::string>(), settings);
```

Le header expose aussi l’inspection et l’inventaire paginé. Les opérations
acceptent annulation et progression. Fichiers et références sont revérifiés
après le dernier callback de publication, sans nouvel observateur pendant la
publication finale. Les écritures externes exigent toujours une coordination ;
il ne s’agit pas d’un snapshot atomique du système de fichiers. Les tests
d’interruption de processus ne démontrent pas la résistance aux coupures
d’alimentation. Le [déplacement lié sur un même système de fichiers](doc:native-workspace-relocation)
est disponible ; déplacements entre systèmes et politiques automatiques par âge/release restent à développer. Les Stores chiffrés protègent les objets
archivés au repos ; les exports et leurs fichiers temporaires ont la sémantique
habituelle d’un workspace en clair.
