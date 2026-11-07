---
id: checkout-retention
title: Retrait des reçus de checkout
description: Guide : Retrait des reçus de checkout.
section: Stockage
order: 60
parent: storage
sourceRevision: 1
---

# Retrait des reçus de checkout terminés {#completed-checkout-receipt-retirement}

Les checkouts terminés conservent un reçu immuable et une racine de révision cible ; leur archive source protège indépendamment les anciens fichiers. Ces racines couvrent aussi switch, reset hard et stash et survivent au GC ordinaire. L’API `syrnck/checkout_retention.hpp` fournit un inventaire paginé et un retrait explicite protégé par sauvegarde.

Par défaut, `checkouts/receipts/ID` et `checkouts/completed/ID` sont retirés ensemble. Ajouter `--include-source-archives` au plan et à l’application retire aussi `archive/checkout/ID` dans la même transaction. Aucun octet de fichier, fichier de travail, tête de branche, index ou dossier privé de récupération n’est supprimé. Le GC ultérieur est séparé.

```text
syrnck native checkout-receipts --limit 100 --verify
syrnck native checkout-receipts --after TRANSACTION_ID --limit 100
syrnck backup create /existing-backup-parent/before-retirement
syrnck backup checkout-retire-plan TRANSACTION_ID --from /existing-backup-parent/before-retirement
syrnck backup checkout-retire TRANSACTION_ID --from /existing-backup-parent/before-retirement --expect-plan PLAN_ID
```


## Couverture et contrôles de publication {#coverage-and-fences}

Sélectionner 1..1024 IDs canoniques uniques. Le plan lie l’emplacement source, la table complète des références, les groupes reçu/cible/source choisis, l’option d’archive et le snapshot exact de sauvegarde. Toute modification impose un nouveau plan.

La sauvegarde distincte doit vérifier descripteur, inventaire et fichiers, puis reconstruire chaque cible et chaque source retirée avec tous ses parents, manifestes et blocs. Les reçus contenant des chemins locaux sont exclus de la sauvegarde ; les racines portables restent présentes. Un Store restauré les affiche avec `retained_only: true` et permet aussi leur retrait protégé.

Un checkout en cours bloque inspection et retrait. Pins pendants orphelins, groupes contradictoires, liens dans les dossiers et données privées de récupération restantes entraînent un refus. Terminer la récupération normale ; le retrait ne nettoie ni n’adopte de fichier inconnu.

Le dernier observateur intervient avant revérification du plan et de la sauvegarde. La vérification finale et le CAS n’ont plus de callbacks, mais restent annulables. Un callback ne peut donc pas faire appliquer un plan sur des preuves périmées. Avant publication, l’annulation conserve toutes les références ; une coupure laisse le groupe ancien ou nouveau entier. Après perte de réponse, réinspecter et refaire le plan.

`SYRNCK_KEY` ouvre la source, `SYRNCK_BACKUP_KEY` la sauvegarde. Conserver copie et clé selon la politique souhaitée. D’autres pins, branches, stashes ou archives peuvent encore garder les mêmes versions : le retrait ne garantit pas un gain immédiat et n’empêche pas un administrateur de supprimer la sauvegarde plus tard.

## Consulter un reçu historique {#historical-receipts}

`native checkout-status --id ID` vérifie le reçu immuable sans exiger l’ancienne sélection HEAD ou une source explicitement retirée. Ces liaisons restent en revanche nécessaires pour récupérer activement un journal. Identité et hash du reçu sont toujours contrôlés.

Si la source a été retirée puis collectée, le statut rend `files: null` et `files_available: false` plutôt que d’inventer des actions. Pour reconstruire les fichiers, utiliser une racine de révision encore conservée ou la sauvegarde. Le reçu seul ne protège pas les octets retirés. Après retrait du reçu, son statut devient une transaction inconnue ; la restauration d’une sauvegarde ne recrée volontairement pas les reçus liés aux workspaces locaux.

## Limites {#validation-and-limits}

Les pages sont limitées à 1000 entrées. Tables de références et rapports gardent leur plafond de 16 Mio. `--verify` contrôle toutes les fermetures affichées sans écrire de fichier. Les payloads sont vérifiés en flux ; les identités d’objets du parcours restent en mémoire.
