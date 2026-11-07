---
id: crash-recovery
title: Interruption et récupération
description: Interruption et récupération
section: Stockage
order: 80
parent: storage
sourceRevision: 1
---

## Inspecter avant récupération {#before-recovery}

Rouvrir le même Store avec sa clé originale et conserver les traces avant nouvelle tentative. L’ouverture ne modifie jamais les fichiers de travail. Pour checkout/switch/merge/replay/rebase/reset/stash en attente, lire `syrnck native checkout-status`, puis choisir `native checkout-recover` pour terminer ou `native checkout-recover --abort` pour restaurer les originaux avant publication. Après publication seul le nettoyage peut finir. Ne pas éditer les slots privés/racines internes. Voir les [états et limites](doc:native-checkout).

Une donnée conservée absente/corrompue est une erreur. Employer une [sauvegarde vérifiée](doc:backups) ou la [réparation par copie](doc:repair-copy) pour les objets absents récupérables, en conservant les traces de corruption. Le GC n’est pas une réparation générale.

## GC interrompu {#interrupted-garbage-collection}

Après un GC dont l’interruption est connue, des métadonnées mortes isolées peuvent survivre à leurs blocs morts empaquetés. Les fermetures conservées restent protégées, mais fsck audite aussi les métadonnées inaccessibles et peut signaler ces dépendances. Avec le même Store/clé : prévisualiser `syrnck gc`, examiner les racines, reprendre la suppression déjà demandée avec `syrnck gc --delete`, puis `syrnck fsck`.

Le GC revalide toutes les fermetures/hashes conservés avant suppression ; une donnée conservée absente/corrompue bloque. Cela termine un nettoyage sans récupérer de version volontairement privée de racines. Ne pas l’appliquer à une erreur inexpliquée. Garder d’abord pins/archives nécessaires et sauvegardes vérifiées.

## Autres opérations et temporaires {#other-operations}

Reprendre pack/compact après correction de l’IO : la configuration active choisit la génération publiée. Une ingestion peut laisser des blocs complets non référencés réutilisables. Ne reprendre une restauration que si destination absente ; sinon vérifier et utiliser le fichier publié sans écrasement. [Export de dossier](doc:export) et sauvegarde peuvent laisser un staging frère privé après kill, sans retrait automatique.

Les `.tmp<random-id>` obsolètes n’entrent pas dans l’inventaire et ne retiennent rien. Aucun nettoyage global automatique ; le retrait validé des packs/index générés inactifs est distinct. La récupération après terminaison de processus n’établit pas la résistance aux coupures, caches contrôleur, FS réseau ou écrivains hostiles.
