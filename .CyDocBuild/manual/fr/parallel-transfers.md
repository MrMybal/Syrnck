---
id: parallel-transfers
title: Transferts parallèles bornés
description: Guide : Transferts parallèles bornés.
section: Transferts
order: 460
parent: transfers
sourceRevision: 1
---

# Transferts parallèles bornés {#bounded-parallel-object-transfers}

L’agent chevauche les requêtes HTTP d’objets avec l’interface multi de libcurl. Lectures/imports du Store, découverte de dépendances et callbacks restent sur le coordinateur appelant ; aucun handle Store n’est partagé avec des threads HTTP.

Défauts : une requête objet et budget de 64 Mio. `--requests` accepte 1..32 ; `--transfer-buffer-mib`, 17..1024. C++ reçoit `net::TransferOptions` après `OperationOptions`, cinquième argument de push/pull. Les appels antérieurs restent séquentiels.

Push négocie les clés absentes par lots de 4096 maximum, vérifie sa source et prépare les payloads sur le coordinateur. Pull découvre les dépendances dans les métadonnées reçues vérifiées. L’ensemble visité évite les téléchargements répétés, même déjà planifiés. Les objets locaux sont lus une fois par `get` vérifié, sans lecture de présence supplémentaire. L’import est validé avant parcours ou publication.

```text
syrnck agent push https://blocks.example.invalid files/MANIFEST asset --requests 4 --transfer-buffer-mib 64 --progress
syrnck agent pull https://blocks.example.invalid asset received --requests 4 --transfer-buffer-mib 64 --progress-json
```


## Budgets et compteurs {#limits-and-counters}

L’ordonnanceur facture la capacité réelle du vecteur d’envoi et un tampon de réponse réservé par requête. Chaque téléchargement réserve 16 Mio logiques ; chaque accusé d’upload réserve 4 Kio et une réponse plus grande échoue. Avant décodage d’un nouvel upload, le budget doit contenir un objet maximum et son accusé : la concurrence réelle peut être inférieure à la demande.

La [compression négociée](doc:network-compression) facture aussi le candidat compressé tant que l’original vit, même s’il est abandonné. Les téléchargements compressés décodent directement dans la réponse réservée. Contextes des codecs hors crédits. Les vecteurs de réponse réservent leur capacité à la première donnée, évitant doubles tampons de croissance. Les crédits restent dus pendant consommation ; la requête terminée est retirée de multi avant cela, les autres restent actives. Attentes multi-poll bornées et réutilisation des connexions sont possibles dans une session.

Les statistiques conservent objets/octets transférés et objets réutilisés. `peak_object_requests` est le maximum de requêtes planifiées simultanées ; `peak_object_buffer_bytes`, le maximum des crédits ; `object_retries`, les tentatives objet supplémentaires ; `object_upload_bytes` et `object_download_bytes`, les corps encodés observés, reprises et réponses partielles/accusés compris.

Ces crédits incluent les capacités réservées, pas seulement les octets reçus ; ils ne sont pas le RSS. Ils excluent allocations internes, libcurl/TLS, caches Store, codecs, clés de parcours, métadonnées et contrôles séquentiels. Un transfert entièrement réutilisé peut avoir zéro requête objet tout en négociant/publiant une référence. Aucun budget mémoire global n’est promis.

## Échec et publication {#failure-and-publication}

Échec non récupérable ou budget de reprises épuisé, réponse corrompue, exception d’observateur ou arrêt coopératif retirent les handles restants et libèrent les payloads hors callbacks libcurl. Les exceptions C sont capturées puis relancées par le coordinateur. Les anciennes racines restent complètes ; les objets vérifiés non référencés se réutilisent.

Les attentes de reprise gardent slot et crédits tandis que d’autres requêtes avancent. Les cibles de débit sont partagées ; voir la [politique](doc:transfer-policy). Les événements de chemins différents peuvent s’entrelacer, avec compteurs propres à chaque requête. La fin d’une requête ne publie rien. Quand tout est présent, la [frontière de publication](doc:transfers#publication-boundary) reste inchangée : écriture locale annulable avant engagement, CAS distant terminé après envoi, code 3 en cas d’incertitude et aucun rejeu automatique.

Le serveur sérialise toujours les accès Store sous mutex pendant que les IO HTTP se chevauchent. Imports et parcours restent coordonnés. Les [journaux](doc:transfer-sessions), [baux d’envoi](doc:server-upload-leases), [inventaires](doc:upload-inventories) et [baux de lecture](doc:server-read-leases) protègent la reprise. Le renouvellement utilise un worker/une connexion de contrôle indépendants, hors compteurs objet ; les inscriptions d’inventaire sont aussi hors crédits. La compression est disponible par [négociation](doc:network-compression). Rétention distribuée et planification multi-source restent à développer.
