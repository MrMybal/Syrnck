---
id: transfer-sessions
title: Sessions de transfert persistantes
description: Guide : Sessions de transfert persistantes.
section: Transferts
order: 590
parent: transfers
sourceRevision: 1
---

# Sessions de transfert persistantes {#persistent-transfer-sessions}

Chaque push/pull inscrit son état local avant envoi des objets. Un autre processus peut reprendre, conserver les blocs vérifiés malgré un GC local et réconcilier un accusé perdu.

`sessions`, `forget`, `prune` sont locaux et ne demandent aucun jeton serveur. Resume utilise les identifiants, autorité privée et options de transfert actuels ; le Store chiffré garde sa clé locale. Ouvrir le Store ne démarre ni réseau ni récupération automatique.

Répéter un transfert retrouve une session inachevée de mêmes direction, endpoint, racine exacte et référence. Resume explicite lit la racine du journal, même après retrait de la référence source. Si la référence distante a changé, une nouvelle commande pull vise la nouvelle racine ; son ancien ID de session reprend l’ancienne.

```text
syrnck agent sessions
syrnck agent resume https://blocks.example.invalid SESSION_ID --requests 4 --progress
syrnck agent forget SESSION_ID
syrnck agent prune
syrnck agent prune --delete
```


## Format et écritures {#format-and-writes}

Les journaux `STORE/transfers/ID.json` utilisent un schéma v3 strict, limité à 16 Kio, avec lecture des versions 1 et 2. Champs : `version`, `id` BLAKE3 stable calculé sur direction/empreinte endpoint/racine/référence, `direction` push ou pull, `endpoint` hash de l’URL canonique, `root` clé exacte, `reference`, `expected` ou null, `state`, `upload_lease` nullable depuis v2 et `read_lease` nullable depuis v3.

URL, jeton, clé, chemin CA et diagnostic ne sont pas enregistrés. Le matching endpoint est exact après normalisation, sans garantie pour toutes les écritures équivalentes. Un autre endpoint est refusé avant HTTP. Comme refs.json, les journaux sont des métadonnées ordinaires locales, sans chiffrement/authentification propre ; l’accès au Store reste la frontière de confiance.

Écriture synchronisée puis remplacement atomique. Sous Unix, création du dossier synchronise son parent et écriture/retrait synchronisent le dossier. Windows garde sa publication atomique existante. Les temporaires non engagés ne changent pas l’état canonique. Verrou exclusif pour journaux/objets/références ; versions inconnues, contenu mal formé, identité incohérente ou chemins liés sont refusés. La durabilité électrique est une question distincte.

Les écritures suivent les frontières d’opération, sans bitmap ni flush par bloc : les objets CAS vérifiés décrivent les payloads terminés. Les requêtes partielles reprennent l’objet entier ; les complets se réutilisent. Reprises, débit et tampons redémarrent à zéro dans le nouveau processus.

## États et protection des références {#states-and-reference-protection}

`transferring` continue si la destination correspond à expected. `publishing` indique une publication imminente ou possiblement achevée ; `uncertain`, une absence de résultat clair ; `conflict`, une destination modifiée à préserver ; `complete`, un reçu historique sans maintien GC ni republication à la reprise.

Le marqueur publishing est durable avant publication. Pour un push publishing/uncertain, une référence distante égale à la racine voulue confirme l’achèvement sans nouveau PUT. Sinon le maintien reste actif et `PublicationUncertain` rend le code 3, même si la destination égale l’ancienne valeur attendue : elle peut avoir été publiée puis remise volontairement. Le CAS initial n’est jamais rejoué aveuglément.

Pull vérifie sa fermeture puis publie par CAS contre expected sauvegardé. Une destination changée est conservée. Après coupure, une racine locale déjà correspondante est vérifiée et marquée complète sans téléchargement. Un bail de lecture enregistré est libéré avant achèvement ; ce nettoyage peut contacter le serveur. Si la référence égale encore expected, publication locale réessayable. Un conflit connu reste bloqué jusqu’à résolution/abandon, sauf racine finale déjà présente.

Les statistiques ajoutent `session` et `reconciled`. True signifie publication confirmée ou ancien reçu complet, avec zéro objet transféré par cet appel. Un reçu complet n’affirme pas que les références ont encore leurs valeurs historiques et ne les recrée pas. Un nouveau transfert achevé réutilise l’ID avec de nouvelles attentes.

L’écriture du reçu final peut échouer après publication : code 3 pour push, erreur ordinaire pour pull. Le marqueur permet alors l’inspection à la reprise. Après envoi du CAS distant, callbacks et annulation restent désactivés jusqu’au reçu ; un arrêt tardif peut réussir. La dernière frontière annulable est l’engagement du marqueur ; l’intervalle marqueur/envoi reste prudemment incertain.

## GC et abandon explicite {#gc-and-deliberate-discard}

Toute session inachevée contribue sa racine au GC. Push exige une source complète vérifiée ; pull conserve les objets accessibles disponibles même si des dépendances manquent. Les autres objets restent collectables. Le GC lit les journaux en flux avec contrôles d’arrêt et conserve les racines nécessaires ; un journal invalide le bloque. L’audit complet de pack peut refuser des métadonnées incomplètes jusqu’à fin du pull ou collecte après abandon.

`agent forget ID` retire le journal et sa protection, sans modifier références ou payloads. Le GC peut ensuite collecter les objets inaccessibles. Un nom de fichier valide peut être abandonné même si son contenu est mal formé. Les reçus complets ne protègent pas leurs racines. Prune prévisualise nombres complets/inachevés ; `--delete` valide tout l’inventaire puis retire uniquement les complets. Une annulation peut en retirer un sous-ensemble, avec nouvel essai sûr. Voir la [rétention sélective](doc:selective-retention).

Tous les GC doivent comprendre ces journaux. Les [baux d’envoi et reçus](doc:server-upload-leases) peuvent confirmer un accusé perdu après changement de référence ; l’expiration permet la collecte serveur et la reprise renégocie/réenvoie depuis la source protégée. Les [baux de lecture](doc:server-read-leases) protègent le pull et les [inventaires](doc:upload-inventories) les objets déclarés. GC distribué et récupération en arrière-plan restent à développer. La liste des sessions matérialise son résultat ; les reçus occupent du disque jusqu’à purge/abandon.
