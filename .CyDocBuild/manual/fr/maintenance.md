---
id: maintenance
title: Maintenance et annulation
description: Guide : Maintenance et annulation.
section: Stockage
order: 180
parent: storage
sourceRevision: 1
---

# Progression et annulation de la maintenance {#maintenance-progress-and-cancellation}

`gc`, `gc --delete`, `pack` et `pack --compact` acceptent `--progress` et `--progress-json` sur stderr et l’annulation coopérative par Ctrl+C. Le rapport final JSON sur stdout reste identique. En C++, utiliser `Store::gc(remove, OperationOptions)` ou `Store::pack(PackOptions, OperationOptions)`.

La préparation vérifie l’annulation pendant la validation des fichiers conservés, la planification de l’inventaire, le tri des clés isolées, l’écriture des packs et leur relecture. Une exception de l’observateur interrompt aussi cette étape. Catalogue, références et fichiers source existants restent intacts ; les temporaires sont retirés au déroulement coopératif. Des packs immuables complets mais inactifs peuvent rester : la prochaine maintenance réussie nettoie leurs fichiers générés. L’ouverture seule ne les active pas.

La portée des compteurs dépend de la phase : `verifying` compte les lectures de payloads décodés ; `planning`, les octets d’enveloppes physiques examinés ; `packing`, les enveloppes vérifiées fournies au writer. Ce dernier n’est pas un compteur d’écritures durables et exclut en-têtes et index. Le total d’empaquetage devient connu après le plan. Le GC peut alterner vérification et empaquetage pour chaque pack partiellement vivant. `publishing` est le dernier callback et contrôle d’annulation avant engagement. Une phase terminée indique la fin de sa préparation ; seul le retour réussi confirme l’opération entière.

Après publication du catalogue, le nettoyage des objets isolés et packs obsolètes se termine sans nouveau callback ni contrôle d’annulation. Sans changement de catalogue, cette frontière précède la première suppression. Elle évite de laisser des métadonnées mortes isolées dont les dépendances empaquetées auraient déjà été retirées. Une annulation tardive peut donc finir avec le code 0 et une période sans progression visible. Le GC en simulation reste annulable tout au long du travail.

Erreurs IO et arrêt forcé peuvent interrompre ce nettoyage engagé. Appliquer la [procédure de récupération](doc:crash-recovery) : les racines conservées restent complètes et un GC interrompu connu peut devoir terminer son nettoyage avant l’audit de toutes les métadonnées inaccessibles. Formats, versions physiques, règles de rétention et verrouillage exclusif ne changent pas.

Un appel IO, codec ou contrôle d’index déjà lancé peut se terminer avant le prochain contrôle d’arrêt. Le GC garde un bit par objet empaqueté, l’empaquetage trie les clés isolées et les métadonnées/parcours gardent leurs allocations distinctes. Progression et annulation ajoutent des compteurs/callbacks de taille constante, sans second inventaire complet.
