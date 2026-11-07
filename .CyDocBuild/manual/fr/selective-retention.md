---
id: selective-retention
title: Rétention des transferts et purge
description: Guide : Rétention des transferts et purge.
section: Transferts
order: 500
parent: transfers
sourceRevision: 1
---

# Rétention sélective des transferts {#selective-transfer-retention-and-receipt-pruning}

Le GC hors ligne traite les transferts locaux inachevés et baux de lecture actifs comme racines ; les autres objets peuvent être collectés pendant interruption. Arrêter le serveur avant une autre ouverture du Store.

Références/pins et push inachevé : vérifier et garder fermeture complète/hashs. Pull inachevé : garder chaque objet accessible disponible. Bail de lecture actif : fermeture complète vérifiée. Upload avec inventaire actif : objets présents déclarés. Upload historique actif : bloquer tout GC jusqu’à expiration/publication/libération. Reçus locaux complets et baux inactifs : aucun payload retenu. Métadonnées de journal/bail invalides : échec avant suppression.

## Pulls incomplets {#incomplete-pulls}

Pull importe les métadonnées vérifiées avant leurs dépendances ; le GC suit ce qui est disponible, blocs après un trou compris. Les absents sont normaux et non marqués ; racine absente seule ne conserve rien. Une association impossible à déduire des métadonnées présentes peut être collectée puis redemandée.

Enveloppe corrompue, métadonnée invalide ou taille d’un bloc présent fausse arrête le GC. Un manifeste complet vérifie son hash entier ; un incomplet ne peut pas encore le faire, mais publication finale l’exige toujours. fsck/pack restent stricts sur les métadonnées orphelines incomplètes.

Le GC vérifie d’abord racines complètes puis ajoute les fermetures partielles aux mêmes bitmap/marques, sans second inventaire complet. Racines, pile et JSON consomment encore de la mémoire ; pas de plafond global. Annulation et point d’engagement catalogue/suppression restent applicables.

Un bail de lecture actif au début conserve prudemment sa racine pour ce GC même s’il expire pendant le parcours. Une expiration déjà observée est durablement fenced avant suppression pour empêcher un retour d’horloge. Les [inventaires](doc:upload-inventories) protègent même avant manifeste ; les anciens baux gardent la protection globale. Verrous locaux/baux ne constituent pas un consensus distribué.

## Purger les reçus locaux complets {#pruning-completed-local-receipts}

agent prune inspecte par défaut ; JSON completed, unfinished, removed, deleted. --delete retire tous les reçus complets, sans critère d’âge, absent du schéma actuel. La progression compte octets de métadonnées lus/retirés, le rapport compte des records.

Deux passages en flux, sans tous les IDs en mémoire : validation totale avant retrait, puis seuls state complete supprimés. Transferring/publishing/uncertain/conflict restent avec leurs protections. Un journal invalide bloque ; agent forget ID permet un abandon explicitement choisi même abîmé/inachevé.

Opération locale sans token/réseau, clé toujours nécessaire au Store chiffré. Références/objets/baux distants intacts. Un reçu retiré n’est plus reprenable par ID ; un nouveau transfert recrée un état ordinaire au lieu de rendre ce reçu.

Annulation/erreur IO ou observateur pendant suppression peut laisser un préfixe retiré ; réessayer est sûr. Le dossier est synchronisé au succès et cette synchronisation est tentée après interruption. Pas de transaction atomique multi-record. Prune/forget ne libèrent jamais un bail distant : expiration ou administration explicite restent nécessaires.

```text
syrnck agent prune
syrnck agent prune --delete --progress
```
