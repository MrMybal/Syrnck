---
id: server-read-leases
title: Baux de lecture serveur
description: Guide : Baux de lecture serveur.
section: Transferts
order: 520
parent: transfers
sourceRevision: 1
---

# Baux de lecture serveur {#server-read-leases}

Un pull négocie automatiquement un bail avant son premier objet manquant avec un serveur compatible. La racine exacte est liée indépendamment de son nom de référence ; une reprise reste possible après déplacement/suppression de celle-ci tant que le bail est actif.

--require-read-lease refuse un serveur non compatible avant download. Une fermeture entièrement locale n’a pas besoin de bail même avec cette option. --require-lease reste réservé aux envois. Les anciens clients peuvent encore faire des GET : c’est une rétention négociée par client, pas une politique d’autorisation.

```text
syrnck serve --read-lease-seconds 3600
syrnck agent pull https://blocks.example.invalid asset received --require-read-lease --requests 4
syrnck agent resume https://blocks.example.invalid SESSION_ID --require-read-lease
```


## Cycle de vie et rétention {#lifecycle-and-retention}

Avant accusé d’un bail neuf/expiré, le serveur vérifie toute sa fermeture et ses hashes de fichiers ; donnée absente/corrompue bloque. Le renouvellement actif évite cet audit répété. L’application hôte sérialise les accès Store ; le serveur fourni utilise mutex et verrou exclusif.

TTL 1..86400 secondes, défaut 3600 ; date UTC serveur arrondie vers le haut d’au plus une seconde. Le client renouvelle au tiers du TTL avec horloge monotone, par requête indépendante annulable qui n’accède pas au Store local ni aux callbacks. Réponses de contrôle limitées à 16 Kio, hors budgets/compteurs objet.

TTL court, validation locale longue, callback, contention ou panne réseau peuvent faire expirer. Une acquisition/relance après expiration change de fence ; un client observant ce changement au renouvellement s’arrête pour reprise. Aucune protection continue n’est garantie pendant le trou d’expiration.

Le GC hors ligne conserve toute la fermeture des baux actifs, laissant les autres objets collectables, même serveur tué/référence retirée. Avant suppression, il inscrit durablement expired et change le fence observé, empêchant un recul d’horloge de réactiver une génération collectée. La simulation n’écrit pas ces transitions ; un bail d’envoi peut imposer une protection supplémentaire.

Après vidage des requêtes, le client annule/joint le renouvellement puis libère le bail avant publication locale. Les objets vérifiés et le journal local inachevé assurent alors la rétention. Le CAS local revérifie fermeture/fichiers, sans publier de version incomplète après annulation, corruption ou changement de destination.

La libération est fenced/idempotente, avec reprises bornées si réponse perdue. Une erreur garde la session inachevée ; resume réessaie avec racine/bail sauvés. Un bail absent/purgé est considéré libéré dès qu’aucune donnée distante n’est requise. Le reçu local complet ne contacte plus le serveur. Un abandon laisse autrement la protection jusqu’à expiration/libération admin ; agent forget ne retire que le journal local.

## Protocole {#protocol}

HTTP/HTTPS authentifié existant. Les capacités annoncent read_leases 1 et read_lease_seconds. GET /v1/reads/ID rend bail/TTL ou 404 ; PUT acquiert/renouvelle avec exactement root et fence, null à création ; POST /release demande exactement fence. Succès : lease et lease_seconds.

IDs et fences sont deux aléas indépendants de 64 caractères hexadécimaux. Schéma strict v1 : version/id/root/fence/expires_at/state, actif/expiré/libéré. Racine immuable ; mauvais fence ou réactivation d’un released = 412. ID purgé recréé = nouveau fence. Ce contrôle concerne renouvellement/libération, pas les GET immuables adressés par hash.

Dossier STORE/reads/ID.json, maximum 4096 enregistrements inactifs compris. Liens, schémas inconnus et métadonnées invalides sont refusés. Écriture atomique synchronisée. Les *.tmp étrangers transitoires peuvent disparaître pendant une attente bornée de 50 ms ; les inconnus persistants échouent et les noms canoniques ne sont jamais ignorés. Métadonnées ordinaires même si objets chiffrés, sans jetons/URL/clés et même confiance filesystem que les références.

Journal local v3 ajoute read_lease nullable pour pull ; v1/v2 restent lisibles. Statistiques ajoutent read_lease à upload_lease, sans prouver leur activité actuelle. Endpoint capacités 404, read_leases 0 ou ancienne réponse valide upload-only permettent le mode ancien sauf option obligatoire. Réponse mal formée/autre erreur propagée ; un bail sauvegardé ne peut être dégradé silencieusement en download non protégé.

## Administration hors ligne {#offline-administration}

Arrêter le serveur avant ouverture CLI de son Store. La clé du Store chiffré est nécessaire, pas le jeton serveur. Prune valide tout l’inventaire borné puis retire les inactifs/expirés. Une libération volontaire peut interrompre la protection d’un lecteur.

Ne jamais lancer le GC d’un ancien binaire ignorant ces racines. La rétention concerne un seul Store serveur, sans réplication ni consensus distribué. Ni démarrage ni renouvellement ne remplacent une sauvegarde contre corruption, retrait externe ou suppression administrative du dossier de baux.

```text
syrnck leases list --read
syrnck leases release ID --read
syrnck leases prune --read
```
