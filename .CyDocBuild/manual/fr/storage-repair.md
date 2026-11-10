---
id: storage-repair
title: Réparer le stockage corrompu sur place
description: Récupérer les copies d’objets absentes ou endommagées en conservant racines et packs intacts.
section: Storage
parent: backups
order: 51
sourceRevision: 1
---

## Prévisualiser et appliquer {#commands}

Utiliser une [sauvegarde vérifiée](doc:backups) distincte comme donneur. Arrêter les écrivains normaux et ouvrir chaque Store avec sa clé. Le Store principal accepte les fournisseurs habituels : clé brute, fichier protégé ou [coffre système](doc:system-vaults#providers) ; `SYRNCK_BACKUP_KEY` ouvre un donneur chiffré indépendamment.

```text
syrnck --store damaged backup repair-storage-plan verified-backup
syrnck --store damaged backup repair-storage verified-backup --expect-plan PLAN_ID
syrnck --store damaged fsck
```

Utiliser l’identifiant `plan` exact de l’aperçu. L’application recalcule l’audit complet et refuse un état périmé avant écriture. Le plan lie chemin source, références, préférences, configuration, octets des copies actives et snapshot vérifié du donneur. Ajouter un objet non référencé ou changer le chiffré invalide aussi l’aperçu. Aucun mode d’application implicite.

Les deux Stores doivent être au repos : aucune reprise de checkout active, aucun transfert incomplet ni bail lecture/envoi non expiré. Une sauvegarde immuable ne peut pas être la cible. Les dossiers d’objets/packs doivent être sans lien ; les objets doivent être des fichiers ordinaires sans lien.

## Couverture et conservation {#coverage}

L’audit vérifie le graphe retenu complet et tous les objets inventoriés, y compris les non-référencés et les deux copies isolée/empaquetée d’un même ID. Il reconstruit les empreintes de fichiers complets par une vue de lecture vérifiée. Une autre copie locale saine est préférée au donneur ; une ancienne sauvegarde n’a donc pas besoin de contenir les données locales récentes encore intactes. Une donnée nécessaire absente/invalide dans les deux entrées bloque avant mutation. Le donneur est entièrement vérifié ; noms et tailles ne font pas autorité.

La réparation importe les objets logiques absents, remplace atomiquement les enveloppes isolées endommagées et reconstruit seulement les packs contenant des payloads corrompus. Les enveloppes saines d’un pack reconstruit sont copiées sans recompression ; toutes les entrées sont conservées, même non référencées. Les nouveaux packs sont relus/vérifiés avant une publication atomique du catalogue. Autres packs actifs, racines, branches, staging, archives, préférences, packs inactifs sans rapport et fichiers inconnus sont conservés. Mode/clé de chiffrement source et identifiants logiques restent inchangés ; le donneur n’est jamais modifié.

Après publication du nouveau catalogue, seuls les anciens fichiers pack/index remplacés sont retirés. Aucun GC n’est exécuté. Les versions archivées restent reconstructibles même si une réécriture d’historique les a retirées des parents courants.

Les rapports indiquent `missing_count`, `corrupt_count`, les nombres de copies endommagées, `packs_to_rewrite` et le `plan` lié. Les tableaux `missing`/`corrupt` affichent au plus 1 000 IDs chacun ; `lists_truncated` signale explicitement les lignes d’affichage omises. Compteurs et engagement physique couvrent toujours tout l’audit. Le succès ajoute `applied`, `imported_objects`, `replaced_loose_copies` et `rewritten_packs`.

## Interruption et reprise {#interruption}

La réparation entière n’est pas une transaction unique. Chaque import d’objet absent, remplacement isolé et bascule finale du catalogue publie atomiquement des octets complets vérifiés. Annulation ou arrêt du processus peuvent laisser un sous-ensemble réparé et d’autres objets endommagés. Obtenir un **nouvel** aperçu et appliquer son nouvel ID pour reprendre. Les réparations terminées ne sont pas annulées et les racines ne sont pas réécrites.

Des packs non publiés ou fichiers temporaires peuvent rester après interruption. Un échec après publication peut aussi laisser les anciens packs devenus inactifs. Un nettoyage ordinaire ultérieur pack/GC peut retirer les fichiers générés inactifs ; la réparation ne balaie pas les fichiers sans rapport. Une erreur IO, d’allocation de sortie ou d’annulation peut suivre des réparations terminées : lire le nouveau plan et `fsck` pour établir le résultat avant de recommencer. La vérification finale est annulable ; les octets publiés restent valides.

## API C++ {#api}

```cpp
#include <syrnck/backup.hpp>

auto preview = syrnck::plan_storage_repair(source, verified_backup, options);
auto result = syrnck::repair_storage(
    source, verified_backup, preview["plan"].get<std::string>(), options);
```

Les API utilisent les handles Store exclusifs existants. Sérialiser l’accès et ne pas réentrer depuis les callbacks de progression. Progression/arrêt suivent les [contrôles d’opération](doc:operations). L’audit utilise des bitmaps pour les objets logiques empaquetés, des empreintes isolées triées et des ensembles d’identités absentes/endommagées. Les buffers de payload sont bornés par objet ; les métadonnées d’identités/empreintes isolées et la pile de parcours restent proportionnelles à l’inventaire. C’est un audit d’intégrité complet hors ligne, pas un scrubber incrémental résident ni un protocole de réplication distant.

## Frontières et qualification {#limits}

La source doit encore s’ouvrir avec configuration, références, catalogue/index de packs, tailles et en-têtes valides. La réparation couvre enveloppes d’objets corrompues et dépendances logiques absentes. Elle ne reconstruit pas les index/configurations perdus, ne récupère pas des octets inconnus inaccessibles, ne renouvelle pas les clés distribuées et n’assure pas de bascule automatique. Une [copie réparée vérifiée](doc:repair-copy) ou [restauration](doc:backups) reste adaptée à une nouvelle destination. L’ancien `backup repair --apply` conserve sa règle stricte limitée aux absences.

Les tests couvrent clair/chiffré, formats isolé/v2/v3, clés de donneur distinctes, archives après réécriture des parents, staging récent, copies dupliquées masquées, enveloppes trop grandes, plans périmés, changements de donneur/racines et annulation après progrès partiel. Des écrivains dédiés sont arrêtés à chaque frontière IO tracée ; un lecteur de production distinct vérifie la récupérabilité sans écriture puis reprend avec un nouveau plan. Ce sont des tests d’arrêt du processus, pas une garantie universelle contre coupure d’alimentation ou écrivain externe hostile.
