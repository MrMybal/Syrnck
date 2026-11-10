---
id: repair-copy
title: Reconstruction d’un dépôt endommagé
description: Guide : Reconstruction d’un dépôt endommagé.
section: Stockage
order: 490
parent: storage
sourceRevision: 2
---

# Reconstruire un Store endommagé ailleurs {#rebuild-a-damaged-store-in-a-verified-new-location}

La copie réparée associe les racines conservées de la source à une sauvegarde intégralement vérifiée. Les objets courants intacts viennent de la source ; les objets absents ou corrompus viennent de la sauvegarde sous leur type/ID exact. Le résultat est un nouveau Store normal vérifié ; les deux entrées restent intactes.

La destination doit être absente, avoir un parent existant et se situer hors des deux entrées. Un dossier existant, même vide, est refusé. Sélectionner ensuite explicitement le nouveau Store : la commande ne remplace ni ne renomme la source abîmée.

```text
syrnck --store damaged backup repair-copy-plan verified-backup
syrnck --store damaged backup repair-copy verified-backup recovered
syrnck --store recovered fsck
syrnck --store recovered native export HEAD recovered-files
```


## Conserver les versions récentes et réparer les anciennes {#preserve-current-history-while-recovering-old-bytes}

Une ancienne sauvegarde n’a pas besoin de posséder toutes les versions actuelles : elle peut fournir un ancien bloc corrompu tandis qu’une nouvelle révision intacte reste copiée depuis la source. Têtes, index figés, archives, tags et autres racines portables sont conservés ensemble.

Le contrôle préalable parcourt la vue combinée, vérifie les identités et tous les hashes de fichiers avant création de la sortie. Un objet nécessaire invalide ou absent des deux côtés bloque la copie. Toute la sauvegarde, y compris reçu, préférences, fermeture et inventaire, est auditée même pour un seul objet donneur. Le plan n’écrit rien et rend `ready`, `missing`, `corrupt`, les compteurs, le hash des références source et l’ID du snapshot donneur.

La copie réimporte les objets vérifiés avec la clé de sortie, conserve les préférences et publie les références. Un fsck complet vérifie le résultat avant son renommage exclusif. Le dernier observateur précède la revérification des références/préférences source et du descripteur donneur : une mutation du callback bloque la publication.

Comme pour une restauration, `workspace/HEAD` et reçus locaux sont omis, mais les racines de données restent présentes. Aucun `backup.json` immuable n’est conservé. Les fichiers de travail, ignorés/non suivis et métadonnées Git ne sont pas copiés comme fichiers ; leurs versions natives conservées restent exportables.

## Objets isolés et enveloppes de packs corrompus {#corrupt-loose-objects-and-packed-envelopes}

La lecture combinée couvre chunks, manifestes, révisions et métadonnées opaques conservées, isolés ou empaquetés. Une enveloppe abîmée peut ainsi être récupérée sans réécrire, supprimer ou mettre en quarantaine le pack original. Les IDs logiques restent identiques ; compression, chiffrement et disposition physique peuvent différer.

La source doit encore s’ouvrir avec sa clé, sa configuration, ses références, son catalogue et ses index valides, ainsi que des tailles/en-têtes de packs conformes. La reconstruction d’index/en-têtes perdus, les tables/configurations abîmées et la substitution en place restent hors de cette opération. Aucun octet inconnu inaccessible n’est récupéré et aucune disposition physique identique n’est promise.

`backup repair --apply` conserve son contrat plus strict : importer seulement les objets absents, avec refus de tout objet existant corrompu. Choisir une copie réparée quand la vue combinée des deux entrées permet de reconstruire le graphe conservé.

## Clés et limites de l’opération {#encryption-and-operation-boundaries}

`SYRNCK_KEY` ouvre la source, `SYRNCK_BACKUP_KEY` le donneur, et `SYRNCK_RECOVERY_KEY` choisit éventuellement une troisième clé indépendante pour la sortie. Sans cette dernière, le CLI conserve la clé principale, ou celle du donneur si lui seul est chiffré. Une entrée chiffrée impose une sortie chiffrée ; aucune clé n’est affichée.

Les entrées sont verrouillées exclusivement et doivent être au repos : pas de checkout actif, transfert incomplet ou bail de lecture/envoi actif non expiré. Il s’agit d’une récupération hors ligne, sans réplication, bascule automatique ou chiffrement de bout en bout.

Annulation et erreurs coopératives avant publication retirent la préparation privée. Un arrêt forcé peut laisser un dossier frère non publié ; la destination nommée est absente ou complète et vérifiée. Réessayer vers une destination absente, ou inspecter le résultat déjà publié. Les entrées corrompues restent disponibles pour analyse indépendante.

Ces API de `syrnck/backup.hpp` fonctionnent aussi sans les composants réseau. Les rapports sont bornés à 16 Mio. Identités et métadonnées de parcours restent en mémoire ; les payloads sont copiés et vérifiés un objet CAS borné à la fois. Les contrôles préalable, de copie et de sortie relisent les données ; aucun débit de production n’est garanti.

```cpp
auto plan = syrnck::plan_repaired_copy(source, backup, operation);
auto result = syrnck::create_repaired_copy(source, backup, new_directory,
                                           destination_key, operation);
```

Pour réparer un Store courant encore ouvrable, la [réparation du stockage](doc:storage-repair) remplace les enveloppes endommagées et les packs touchés sans créer une copie complète.
