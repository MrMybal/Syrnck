---
id: archive-retention
title: Retrait des archives
description: Guide : Retrait des archives.
section: Stockage
order: 20
parent: storage
sourceRevision: 2
---

# Retirer des archives avec une sauvegarde vérifiée {#retire-native-archives-with-a-verified-backup}

Les archives natives conservent les versions précédentes après reset, rebase, checkout, amend ou remplacement de l’index. Les racines `native-fetch/REVISION` conservent aussi l’historique récupéré. Le GC garde leurs fermetures tant qu’elles restent référencées.

L’API de `syrnck/retention.hpp` et `backup retire-plan` / `backup retire` retirent les sélections dans une seule transaction. Une sauvegarde indépendante doit d’abord vérifier son descripteur, son inventaire et tous les fichiers : parents des révisions, manifestes et blocs compris. Le retrait ne supprime aucun objet et ne lance pas le GC.

Reprendre exactement le champ `plan` de l’aperçu. Ce plan lie la table complète des références, la sélection triée et le snapshot de sauvegarde vérifié. Une modification de ces éléments impose un nouvel aperçu. L’application revérifie la couverture et publie par comparaison-échange ; une annulation avant publication conserve toutes les racines.

Choisir de 1 à 1024 sélecteurs uniques. Les archives simples reconnues sont `reset`, `reset-index`, `rebase-index`, `stage-index`, `squash`, `amend`, `checkout`, `fetch`, `publish`, `bisect`, `stash` et les historiques détachés, ainsi que les racines exactes `native-fetch/REVISION`. Les têtes, tags actifs, index et verrous d’assets sont refusés.

Pour les archives structurées, sélectionner le groupe entier : `archive/branches/ID` retire ensemble le descripteur, la cible et l’éventuel index figé ; `archive/tags/ID` retire ensemble le descripteur, la cible et l’éventuelle annotation. `native branch-archives` et `native tag-archives` acceptent `--after ID`, `--limit N` et `--verify` pour retrouver les groupes. Un membre `/target`, `/record`, `/index` ou `/annotation` ne peut pas être retiré seul. Les groupes incomplets, membres inattendus et liaisons d’annotation/base d’index incorrectes sont refusés. La sauvegarde doit contenir les références exactes du groupe ; les mêmes blocs sous d’autres noms ne constituent pas une recette de restauration.

Les lignes du plan ajoutent `name`, la table `roots`, `kind: branch-group` ou `tag-group`, et la révision cible. `removed_references` compte les références développées. La sortie reste inférieure à 16 Mio, échappement JSON inclus. Les groupes et racines simples peuvent partager une transaction. Les branches ou tags recréés sous l’ancien nom, HEAD, les fichiers de travail et les autres références restent intacts.

Le dépôt principal utilise `SYRNCK_KEY`, la sauvegarde secondaire `SYRNCK_BACKUP_KEY` ou aucune clé si cette variable est absente. Les clés peuvent différer. Les deux dépôts restent verrouillés ; toute récupération de checkout doit être terminée auparavant. D’autres racines peuvent encore conserver les versions : aucun gain disque immédiat n’est garanti.

Après collecte locale, restaurer la sauvegarde dans un dépôt séparé avec `backup restore`, puis appeler `native branch-restore ID` ou `native tag-restore ID`. Modifier directement les références de la sauvegarde invalide son descripteur. Conserver la sauvegarde et sa clé selon la politique voulue ; leur suppression ultérieure reste administrable. Les politiques automatiques par âge/release et le GC distribué restent à développer.

Les [archives de workspace](doc:native-workspace-archives) utilisent aussi un sélecteur de groupe entier : `archive/workspaces/ID`. Le retrait conserve la même exigence de sauvegarde exacte et retire ensemble descripteur, HEAD, révision physique et toutes les anciennes références du contexte. Aucun membre ne peut être retiré seul. `native workspace-archives` accepte les mêmes options de pagination/vérification ; les lignes du plan utilisent `kind: workspace-group`. Après restauration de la sauvegarde dans un Store séparé, utiliser `workspace-archive-restore-plan` puis `workspace-archive-restore`.

Le dernier observateur de progression intervient avant la revérification finale. Cette dernière et le CAS n’appellent plus d’observateur, afin qu’un callback modifiant les références ou la sauvegarde n’autorise pas un retrait périmé. Voir aussi le [retrait des reçus de checkout](doc:checkout-retention).

```text
syrnck native archives --kind reset
syrnck backup create /existing-backup-parent/before-retirement
syrnck backup retire-plan archive/reset/REVISION --from /existing-backup-parent/before-retirement
syrnck backup retire archive/reset/REVISION --from /existing-backup-parent/before-retirement --expect-plan PLAN_ID
syrnck gc
```


```text
syrnck native branch-archives --verify
syrnck native tag-archives --verify
syrnck backup create /existing-backup-parent/before-retirement
syrnck backup retire-plan archive/branches/BRANCH_ID archive/tags/TAG_ID --from /existing-backup-parent/before-retirement
syrnck backup retire archive/branches/BRANCH_ID archive/tags/TAG_ID --from /existing-backup-parent/before-retirement --expect-plan PLAN_ID
```
