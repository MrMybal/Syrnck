---
id: transfer-policy
title: Reprise et objectifs de débit
description: Guide : Reprise et objectifs de débit.
section: Transferts
order: 580
parent: transfers
sourceRevision: 1
---

# Reprises et cibles de trafic partagées {#retry-policy-and-shared-traffic-targets}

Push et pull peuvent reprendre les erreurs transitoires et réguler le trafic objet dans chaque direction. Le service et `SYRNCK_TOKEN` doivent être configurés ; `--ca` fournit une autorité privée. Le CLI traduit ses messages, garde son JSON anglais et ne persiste aucun identifiant secret dans cette politique.

```text
syrnck agent push https://blocks.example.invalid files/MANIFEST asset --requests 4 --retries 3 --upload-kib-s 4096
syrnck agent pull https://blocks.example.invalid asset received --requests 4 --download-kib-s 8192 --progress-json
```


## Frontières des reprises {#retry-boundaries}

`--retries` vaut 0..8 tentatives supplémentaires, 3 par défaut ; zéro désactive les reprises applicatives. Chaque requête possède son budget. Opérations concernées : GET refs, POST missing et GET/PUT d’objets. PUT est idempotent par son hash vérifié : un accusé perdu peut entraîner le renvoi complet. Un download recommence l’objet, pas une plage ; aucune réponse partielle n’est importée.

HTTP 408, 429, 500, 502, 503, 504 et erreurs DNS/connexion/envoi/réception/réponse vide/partielle/timeout sont repris dans le budget restant. Autres statuts, dont 401/403/404/409/501, échec TLS, protocole/taille/objet/dépendance invalide, erreur de callback et annulation échouent immédiatement. Le PUT final de référence n’est jamais repris automatiquement, même avec statut transitoire ou réponse perdue.

Attente initiale 250 ms, doublée à chaque tentative, plafond 30 secondes. Retry-After entier ou date HTTP valide peut augmenter l’attente jusqu’au même plafond ; un champ mal formé est ignoré. L’attente utilise ensuite une horloge monotone, sans jitter aléatoire. Les erreurs d’identité et de payload ne sont pas masquées.

Une requête en attente garde slot et tampons facturés mais quitte le multi libcurl. Les autres avancent ; aucun budget ne peut être contourné. Vecteur d’envoi et capacité de réponse sont réutilisés. L’annulation réveille attentes de reprise et débit. Callbacks sur le coordinateur, compteurs remis à zéro par tentative.

`object_retries` compte seulement les nouvelles tentatives objet, pas les contrôles. Les compteurs logiques de succès ne comptent chaque objet qu’une fois, hors tentatives partielles/répétées ; un échec final n’a pas de statistiques de succès. Avec compression, progression/débit et compteurs réseau portent sur les corps encodés. Erreurs de codec/en-têtes ne sont pas reprises ; download réinitialise son décodeur et upload conserve sa trame.

La [publication](doc:transfers#publication-boundary) garde son point d’engagement : aucun callback/arrêt coopératif après envoi du CAS. L’incertitude rend 3 et demande inspection. Une nouvelle session capture une nouvelle référence attendue ; une session inachevée garde l’ancienne et utilise la [réconciliation](doc:transfer-sessions) sans rejeu aveugle.

## Cibles de débit {#traffic-targets}

`--upload-kib-s` et `--download-kib-s` utilisent des Kio/s (1024 octets/s), plage 0..1073741824 ; zéro, valeur par défaut, signifie illimité. Ce sont des cibles moyennes agrégées de corps d’objets pour l’opération entière, partagées entre requêtes, reprises et lots, sans multiplication par `--requests`.

Chaque handle reçoit sa part des limites libcurl. Le coordinateur cumule les corps de toutes les tentatives et règle la suite sur un budget commun de temps écoulé, soldé avant publication : de petits objets n’obtiennent pas chacun une nouvelle tolérance de démarrage. Le budget d’un push couvre tous ses lots. Reprises, travail local et inactivité comptent dans le temps ; le débit réel peut rester inférieur à la cible.

Ce n’est pas un policier instantané de paquets : rafales courtes, buffers socket et ordonnancement sont permis. En-têtes HTTP, TLS, requêtes de contrôle et CAS final sont exclus. Corps des tentatives échouées/répétées et accusés sont inclus. Une opération annulée/échouée peut terminer avant compensation de sa dernière rafale ; aucune session suivante n’hérite du budget.

Le délai objet couvre le temps maximal d’un objet à la cible/concurrence choisie plus 120 secondes. Connexion : 15 secondes ; contrôles et CAS : 120 secondes. Aucun délai global n’est garanti et les timeouts proxy/serveur restent applicables.

C++ expose `max_retries`, `retry_delay_ms` (1..30000), `upload_bytes_per_second`, `download_bytes_per_second`. Un débit non nul vaut au moins `max_requests` octets/s et au plus 1 Tio/s. Passer les options après OperationOptions pour push/pull, en deuxième argument de refs. Refs permet les reprises mais n’a pas de trafic objet à limiter.

## Reprise après interruption {#recovery-and-validation}

Relancer renégocie la présence distante ou vérifie les objets locaux et réutilise les imports terminés. Les [sessions persistantes](doc:transfer-sessions) gardent racine et référence attendue, et protègent sélectivement les objets pendant le GC jusqu’à achèvement ou abandon. Budgets de reprises et crédits de débit ne persistent pas. Baux d’[envoi](doc:server-upload-leases) et de [lecture](doc:server-read-leases) assurent leurs protections respectives tant qu’ils restent actifs. La reprise automatique en arrière-plan reste à développer.
