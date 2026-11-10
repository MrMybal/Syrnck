---
id: backups
title: Sauvegardes, restauration et réparation
description: Guide : Sauvegardes, restauration et réparation.
section: Stockage
order: 50
parent: storage
sourceRevision: 5
---

# Sauvegardes vérifiées et restauration {#verified-offline-backups-restore-and-missing-object-repair}

L’API `syrnck/backup.hpp` crée un snapshot logique de toutes les racines conservées : parents des révisions, anciennes têtes, tags, index figés, stashes et compteur des verrous d’assets. Les hashes complets des fichiers sont contrôlés avant publication ; la source reste intacte.

La destination doit être un nouveau dossier, hors du Store source, avec un parent existant. Même vide, une destination existante est refusée. Création et restauration préparent un dossier frère privé, le synchronisent puis le publient par renommage exclusif. Une annulation ou exception antérieure ne rend pas visible de destination partielle. Un arrêt forcé peut laisser un frère `.syrnck-export-*` non publié : il n’est pas une sauvegarde valide. Réessayer crée une nouvelle préparation ; le nettoyage automatique reste à développer.

Arrêter le serveur. Le verrou exclusif exclut les écrivains normaux. Checkout actif, transfert incomplet et bail de lecture/envoi actif non expiré bloquent la création ; les terminer ou retirer explicitement auparavant. Les baux d’assets conservent leurs dates d’expiration.

Ce n’est pas une image du système de fichiers : fichiers de travail, modifications non préparées, ignorés/non suivis, métadonnées Git, objets inaccessibles, journaux de transfert et dossiers privés de checkout ne sont pas copiés. Les liaisons absolues `workspace/HEAD` et reçus de checkout terminés sont omis, mais les racines de versions source/cible restent présentes. La restauration produit un Store indépendant ; exporter ensuite la révision choisie pour récupérer les fichiers, puis recréer la liaison au workspace séparément.

```text
syrnck --store project/.syrnck backup create /existing-backup-parent/snapshot-001
syrnck --store /existing-backup-parent/snapshot-001 backup verify
syrnck --store /existing-backup-parent/snapshot-001 backup restore /existing-parent/recovered-store
syrnck --store /existing-parent/recovered-store fsck
syrnck --store /existing-parent/recovered-store native export heads/main /existing-parent/recovered-files
```


## Vérifier et changer de clé {#verify-and-change-encryption-keys}

Le descripteur canonique `backup.json` enregistre les hashes des références et préférences, le nombre d’objets, les octets décodés, la date et les références omises. `backup verify` contrôle ce reçu, toutes les dépendances, tous les hashes de fichiers et l’inventaire complet. Changer références/préférences ou ajouter un objet non référencé invalide le contrôle. Le reçu n’est ni signé ni authentifié par une autorité externe : protéger les sauvegardes contre les modifications.

`SYRNCK_KEY` ou le [fournisseur de fichier de clé protégée](doc:key-files#providers) ouvre le Store principal ; `SYRNCK_BACKUP_KEY` choisit éventuellement la clé de destination de create/restore. Sans cette dernière, le CLI conserve la clé d’une source chiffrée ; une source en clair reste en clair. L’API C++ exige une clé de destination pour une source chiffrée. Les IDs restent identiques après rechiffrement ; changer de clé ne révoque pas les anciennes copies et ne masque pas les égalités entre IDs.

Pour vérifier une sauvegarde, fournir sa propre clé dans `SYRNCK_KEY`. Pour la restaurer avec une autre clé, fournir cette dernière dans `SYRNCK_BACKUP_KEY`. Une clé contient 32 octets, encodés en 64 caractères hexadécimaux ; la conserver hors de Git et des chemins de sauvegarde. Descripteur et références restent visibles. Il ne s’agit ni de chiffrement de bout en bout, ni de dérivation de mot de passe, ni de coffre système ou rotation distribuée.

La restauration vérifie entièrement la sauvegarde, importe les objets décodés dans de nouvelles enveloppes, conserve références et préférences, puis vérifie le résultat. Le Store restauré n’a pas de reçu `backup.json`, afin de pouvoir évoluer normalement. Conserver l’original jusqu’au contrôle des fichiers exportés.

## Réparer les dépendances manquantes {#repair-missing-retained-dependencies}

Le [retrait d’archives](doc:archive-retention) utilise une sauvegarde vérifiée, un aperçu attendu et une transaction de références ; le GC reste explicite.

Pour la réparation, `SYRNCK_KEY` ouvre la destination abîmée ; `SYRNCK_BACKUP_KEY` ouvre la sauvegarde donneuse, en clair si cette variable est absente. Les clés peuvent différer.

Sans `--apply`, la commande est un plan en lecture seule : `ready`, `missing`, `corrupt`, `references_hash`. Un blocage rend le code 1 et du JSON ; appliquer un plan bloqué échoue avant toute réparation partielle. La sauvegarde est vérifiée complètement. Le plan parcourt les racines portables de la destination et n’utilise la sauvegarde qu’en cas d’objet absent. Il vérifie tous les hashes de fichiers dans cette vue combinée avant toute écriture. Une ancienne sauvegarde peut réparer un ancien asset tout en gardant des révisions récentes intactes qu’elle ne possède pas. Un objet absent des deux côtés bloque l’opération.

L’application importe uniquement les objets absents, chacun par enveloppe complète atomique. Une annulation ou coupure peut laisser un sous-ensemble réparé ; relancer termine de façon idempotente. Aucun objet existant ni référence n’est remplacé ou retiré. Sortie bornée à 16 Mio, ensembles d’identités encore en mémoire.

Tout objet existant corrompu bloque la réparation et reste disponible pour inspection. Cette API ne remplace pas les objets corrompus, ne reconstruit pas les packs perdus ni les configurations/références abîmées. La [copie réparée](doc:repair-copy) reconstruit plutôt un nouveau Store à partir des versions récentes intactes et de la sauvegarde, sans modifier les deux entrées.

```text
syrnck --store damaged-store backup repair snapshot-001
syrnck --store damaged-store backup repair snapshot-001 --apply
syrnck --store damaged-store fsck
```


## Réparer les copies physiques corrompues {#physical-repair}

La [réparation du stockage sur place](doc:storage-repair) utilise un aperçu lié à l’état pour restaurer les enveloppes isolées corrompues et reconstruire les packs touchés, en gardant références et données récentes intactes. `backup repair-storage-plan` audite chaque copie physique active ; `backup repair-storage --expect-plan ID` applique. Réparation des seules absences et copies réparées vérifiées conservent leurs contrats.

## Limites {#validation-and-limits}

La vérification complète relit volontairement les données conservées. Le parcours ne dispose pas encore d’inventaire sur disque. Ces opérations ne promettent pas une durabilité universelle après panne d’alimentation. Réplication en direct, bascule multi-serveur, scrubbing programmé, rétention distribuée et remplacement automatique des données corrompues restent des fonctions distinctes à développer.

## Snapshots portables des workspaces liés {#linked-workspaces}

Sauvegardes et copies réparées gardent chaque sélection liée dans des métadonnées
portables avec sa révision exacte conservée et son index figé. Les chemins
absolus registration/HEAD et reçus sont omis. Sans capacités requises, ces
exclusions utilisent le marqueur v2 ; avec exigences de lecteur, v3. Les anciennes
v1/v2 restent prises en charge.

Après restauration du Store dans un nouveau dossier principal, lancer
`native workspace-restore NOM NOUVEAU_DOSSIER [--branch NOM | --detach]`.
Le défaut reprend la branche originale si sa tête correspond encore et qu’aucun
autre workspace ne la sélectionne. Sinon choisir un autre nom ou HEAD détaché.
L’index figé est matérialisé s’il existe ; HEAD garde la révision commitée.
Les modifications de travail non préparées ne sont pas sauvegardées. Une
exportation complète interrompue se reprend seulement si tous ses fichiers
correspondent encore. Voir [la restauration](doc:native-workspaces#restore).

## Conservation des exigences de lecteur {#reader-features}

Un Store avec capacités requises produit un marqueur v3, liant sa liste non vide
`required_features` à la configuration sauvegardée. La vérification refuse une
exigence modifiée ou retirée, y compris un marqueur rétrogradé en v1/v2 alors que
le Store exige encore des capacités. Les sauvegardes classiques v1/v2 restent
acceptées. Restauration et copies réparées conservent les exigences source,
avec leur clé de chiffrement indépendante et les règles d’export existantes.
Voir [la sémantique des exigences](doc:protocol#required-store-features).
