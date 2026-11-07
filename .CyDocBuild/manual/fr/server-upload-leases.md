---
id: server-upload-leases
title: Baux d’envoi persistants
description: Guide : Baux d’envoi persistants.
section: Transferts
order: 530
parent: transfers
sourceRevision: 1
---

# Baux d’envoi persistants {#persistent-server-upload-leases}

Les nouveaux clients protègent automatiquement leurs envois si le serveur annonce l’extension. Les baux survivent aux redémarrages et protègent pendant le GC hors ligne ; ils expirent sans renouvellement, puis les références publiées prennent le relais au succès.

Le serveur utilise son authentification partagée ou les [identités configurées](doc:server-access), une écoute loopback et un proxy HTTPS pour le distant. Les Stores chiffrés ont leur propre clé. Texte bilingue, protocole anglais. Cette extension concerne l’upload ; les [baux de lecture](doc:server-read-leases) protègent les pulls séparément.

```text
syrnck --store server-data serve --lease-seconds 3600 --require-lease
syrnck agent push https://blocks.example.invalid files/MANIFEST asset --require-lease
```


## Durée, GC et administration {#lifetime-gc-and-administration}

Défaut 3600 secondes, réglage 1..86400, arrondi d’expiration au plus une seconde. Renouvellement au tiers du TTL via horloge monotone client, sans synchronisation des horloges ; le serveur conserve l’expiration sur son horloge murale à maintenir correctement. Recul peut prolonger, avance expirer. Avant suppression GC, une expiration observée est inscrite durablement avec nouveau fence ; recul ultérieur ne réactive pas une génération collectée. Un refus de publication expirée l’enregistre aussi ; pas d’écriture en simulation.

Le coordinateur planifie les renouvellements durant attentes de reprise/débit. Un worker au plus, HTTP indépendant pour ne pas bloquer les objets, sans accès Store ni callbacks utilisateur. Résultat/fence contrôlés par le coordinateur ; worker annulé/joint en sortie et terminé avant publication. Sa connexion/tampons de contrôle sont hors compteurs objet. IO locale, callback et contrôles synchrones peuvent retarder la planification : choisir un TTL suffisant, aucun maintien pendant une pause arbitraire n’est garanti.

Un ancien bail actif non expiré bloque tout GC du Store, simulation comprise. Les [inventaires paginés](doc:upload-inventories) ne gardent que les objets déclarés. Les expirés/libérés/publiés ne conservent rien ; expiration ne supprime pas les payloads sans GC explicite. Maintenance hors ligne, serveur tenant son verrou exclusif.

Après arrêt, list/release/prune administrent les baux. Release révoque sans modifier références/payloads ni reçu publié. Prune valide puis retire inactifs et générations d’inventaire obsolètes ; actifs conservés. Retirer un reçu peut réduire les preuves d’un client déconnecté, jamais autoriser un ancien CAS.

Au plus 4096 enregistrements reçus compris, création bloquée à capacité ; 16 Kio par record, 1 Mio par page d’inventaire. La liste matérialise cet inventaire borné. Métadonnées mal formées/liées refusées. Tous les moteurs de GC doivent connaître l’extension.

```text
syrnck --store server-data leases list
syrnck --store server-data leases release LEASE_ID
syrnck --store server-data leases prune
```


## Fencing et reprise {#fencing-and-resume}

Le client persiste un ID aléatoire avant création serveur ; le bail lie racine exacte, référence et ancienne valeur attendue. Renouvellement vivant garde le fence, réactivation/recréation en change. Toute publication louée porte ce fence ; expiré/libéré/périmé est refusé avant publication, même après purge/recréation d’ID. C’est une coordination locale sous verrou, pas un consensus multi-serveur.

Un changement de fence pendant renouvellement interrompt pour resume : une requête de présence avant un trou de rétention n’est plus fiable. Des objets collectés peuvent être renvoyés ; le journal local garde la source.

Le serveur vérifie toute la fermeture, applique le CAS sauvegardé puis écrit un reçu durable published. Deux écritures atomiques distinctes : une coupure entre elles laisse bail actif et référence possiblement publiée. La confirmation à la reprise peut enregistrer le reçu d’une racine correspondante sans nouveau PUT de référence.

Le reçu lie le descripteur d’origine. Répéter sa publication accuse le reçu, sans modifier une référence déplacée ensuite ; la reprise peut donc réussir après un autre écrivain. Sans reçu et référence non correspondante, la publication reste incertaine. Le reçu seul ne conserve pas d’ancienne version après retrait des références.

L’annulation laisse le bail jusqu’à expiration/libération ; agent forget ne contacte pas le serveur. Les records ont IDs/fences/clés d’objets, pas jetons ni clés de chiffrement. En mode configuré, leur propriété est isolée par principal ; le mode partagé historique conserve son identité partagée.

## Protocole et compatibilité {#protocol-and-compatibility}

Capacités : upload_leases 1, lease_seconds, éventuellement upload_inventories 1. GET uploads/ID rend lease/TTL ou 404 ; PUT crée/renouvelle avec root/reference/expected/fence et inventory true optionnel. PUT inventory/INDEX inscrit une page avant présence ; POST confirm avec fence confirme une référence correspondante sans la publier. PUT refs/NAME avec upload {id,fence} applique/réconcilie le CAS lié.

Création fence null, idempotente pour descripteur identique ; renouvellement avec dernier fence. Requêtes/réponses de contrôle 16 Kio, grandes entrées rejetées avant parsing. STORE/uploads/ID.json v1 strict : version,id,reference,root,expected,fence,expires_at,state active/expired/published/released, avec écriture synchronisée atomique. V2 ajoute inventory_pages/inventory_hash ; inscriptions séparées au plus 1 Mio.

412 refuse publication expirée/libérée/fenced ; 428 refuse absence de bail obligatoire. Le client les interprète comme rejets connus seulement sur son chemin loué ; l’ambiguïté reste code 3 et aucun PUT final n’est repris automatiquement.

404 des capacités ou upload_leases 0 permet l’ancien mode sauf protection exigée. Erreur réseau, schéma invalide ou perte de support d’un bail enregistré échoue sans dégradation. --require-lease client refuse avant upload ; côté serveur, il refuse le PUT final non loué, pas les PUT d’objets historiques. Cette option concerne push/resume de push, pas pull.

Les journaux locaux écrivent v3 avec upload_lease nullable hérité de v2 et read_lease ; v1/v2 lisibles. Les statistiques portent ces IDs historiques, pas la preuve de validité actuelle. Objets logiques, pointeurs Git et ABI C restent identiques.
