---
id: transfers
title: Transferts et publication
description: Guide : Transferts et publication.
section: Transferts
order: 400
sourceRevision: 1
---

# Transfert, progression et publication {#transfer-progress-cancellation-and-publication}

L’agent HTTP ponctuel accepte `OperationOptions` : `Remote::refs`, push et pull utilisent un jeton d’arrêt et un observateur synchrone. Le CLI garde les statistiques finales sur stdout. Utiliser un service réellement configuré, `SYRNCK_TOKEN` et, pour le Store local chiffré, sa propre `SYRNCK_KEY`. Événements et progression ne contiennent ni URL, ni jeton, ni clé ; ils identifient des chemins relatifs du protocole.

La phase `transferring` compte corps de requête et de réponse HTTP d’une tentative ; les compteurs repartent par requête et tentative. Le total peut être inconnu puis devient connu avec Content-Length et exact au succès de la requête. Les contrôles au premier plan rendent aussi des événements ; les renouvellements de baux en arrière-plan n’appellent pas l’utilisateur. Fin de requête ne signifie ni import vérifié ni référence publiée. `transferred_bytes` final ne compte qu’une fois les payloads logiques des nouveaux objets, hors contrôles et objets réutilisés. Texte traduit, clés/phases JSON anglaises.

Libcurl peut arrêter une requête en cours ou une attente inactive. Des contrôles ont aussi lieu entre requêtes, objets, validations et écritures. Les callbacks sont sur le thread appelant ; leurs exceptions sont interceptées à la frontière C, font interrompre la requête puis sont relancées en C++, jamais à travers libcurl. IO/codecs/résolution déjà lancés peuvent finir avant le contrôle suivant : l’arrêt est coopératif, pas un délai global immédiat.

Push vérifie toute la fermeture source et les hashes complets avant envoi, négocie la présence par lots puis transmet les absents. Pull vérifie chaque objet avant import. Une interruption laisse des objets complets réutilisables ; les réponses partielles ne sont pas importées. Un manifeste téléchargé peut temporairement manquer de dépendances, faisant échouer un audit de métadonnées inaccessibles ; les anciennes racines restent reconstructibles. La [session locale](doc:transfer-sessions) inachevée protège la source ou les objets reçus. Un [bail de lecture](doc:server-read-leases) protège la racine distante exacte jusqu’à disponibilité locale ; il est libéré avant publication locale.

```text
syrnck agent push https://blocks.example.invalid files/MANIFEST asset --progress
syrnck agent pull https://blocks.example.invalid asset received --progress-json
```


## Authentification serveur et corps HTTP {#server-authentication-and-request-bodies}

Le serveur reçoit le corps avant contrôle d’identité, sous limite de 16 Mio et timeout de lecture de 30 secondes. Il exige ensuite exactement un en-tête `Authorization: Bearer <token>` avant parsing JSON applicatif, décompression, accès Store ou publication. Identifiants absents, faux ou dupliqués donnent 401 et `WWW-Authenticate: Bearer realm="Syrnck"`, même pour une route inconnue. Recevoir d’abord le corps évite un reset TCP Windows masquant le refus.

Ce contrat concerne les corps bien formés complètement reçus avec Content-Length ou framing chunked. Erreurs de framing, codage transport non géré et dépassement de taille peuvent précéder l’authentification. `100 Continue` permet l’envoi, sans autoriser l’accès. Les connexions sont fermées après erreur HTTP. La réception occupe encore un worker ; quotas de déploiement et contrôle d’admission restent à développer.

## Frontière de publication {#publication-boundary}

Pull vérifie l’annulation jusqu’à validation finale et écriture atomique locale ; un arrêt avant celle-ci garde l’ancienne référence. Push émet `publishing`, contrôle l’arrêt avant préparation du CAS distant et pendant l’écriture du marqueur de journal. Un arrêt à ce callback empêche l’envoi.

Après engagement du marqueur, le CAS est envoyé et terminé même si l’arrêt arrive ensuite. Une coupure entre marqueur et envoi est traitée comme publication incertaine. Annuler un CAS déjà envoyé ne défait pas une publication serveur. Aucun callback ne suit ce point d’engagement ; un Ctrl+C tardif peut rendre le code 0. Le timeout final reste 120 secondes. Le serveur vérifie la fermeture complète avant publication ; un conflit 409 est un refus normal.

Réponse perdue ou statut ambigu : C++ lance `net::PublicationUncertain`, le CLI affiche un diagnostic, garde stdout vide et rend le code 3, même si un arrêt est demandé. Le serveur peut déjà avoir publié : inspecter sa référence. Les erreurs de transport, serveur et réponses inattendues sont interprétées prudemment ; 401/403/404/409 restent des erreurs ordinaires. La reprise consulte le journal et confirme une racine correspondante sans rejouer aveuglément le CAS. Un [reçu d’envoi publié](doc:server-upload-leases) peut confirmer le succès même après déplacement ultérieur de la référence. Un rejet loué 412/428 garde la session pour un nouvel essai volontaire.

L’annulation coopérative ordinaire rend 130, sans retirer objets vérifiés ou références existantes. Formats et base HTTP v1 restent compatibles ; les baux utilisent une extension négociée.

## Ressources et limites {#validation-and-remaining-work}

Les requêtes d’objets sont séquentielles par défaut ; [parallélisme borné](doc:parallel-transfers), [reprises et débit](doc:transfer-policy) et [journaux persistants](doc:transfer-sessions) sont optionnels ou explicitement configurés. Push matérialise encore fermeture et liste triée ; pull conserve un ensemble visité et une file de parcours. Le suivi local ne constitue pas une synchronisation continue complète ; la rétention distribuée reste à développer. L’intégration Git dispose séparément de ses [catalogues et hooks](doc:git-assets).
