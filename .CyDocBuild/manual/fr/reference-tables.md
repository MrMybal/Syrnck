---
id: reference-tables
title: Tables de références
description: Guide : Tables de références.
section: Stockage
order: 480
parent: storage
sourceRevision: 1
---

# Tables de références {#reference-table-parsing-and-listing}

Les grands inventaires de rétention Git peuvent garder de nombreux manifestes indépendants. Le chargement et la liste évitent les recherches quadratiques sans modifier le stockage ou HTTP.

`refs.json` et `Remote::refs` passent par un objet JSON arborescent avant validation des noms et clés dans `Refs`. La sérialisation ajoute directement les entrées triées et uniques à l’objet JSON ordonné. CLI, serveur et writer atomique partagent cet encodage. Ordre des entrées/champs et octets restent identiques, sans nouveau format, ID ni ABI.

## Validation du JSON {#json-validation}

Un garde de profondeur SAX précède le DOM normal, sans construire de second DOM. Il évite les recherches répétées dans les parents de l’ancien callback de parsing. Les métadonnées immuables conservent JSON ordonné et validation canonique.

La profondeur maximale de 128 niveaux reste inchangée, y compris pour les conteneurs vides. Syntaxe, UTF-8, nombres, noms, types, hashes et clés sont contrôlés. L’ordre des champs est libre dans les tables de références et les noms JSON dupliqués gardent le comportement antérieur de dernière valeur. Un document invalide ne remplace pas la table persistée. La limite de 16 Mio s’applique en lecture comme en écriture.

Le parsing coûte O(N log N), l’encodage d’une table triée O(N), hors longueurs des clés et octets de sortie. Cela ne rend pas tous les objets JSON ordonnés linéaires. Deux passes lexicales ajoutent du travail aux petits documents ; DOM et `Refs` coexistent pendant conversion. Le document complet est relu à chaque accès. Index persistant, journal incrémental et budget mémoire global restent à développer.

## Coûts et limites en exécution {#measured-cli-listing-windows-x64-4-november-2026}

Les mesures synthétiques ne garantissent pas les performances de production. Tenir compte du nombre d’objets, des métadonnées, de la disposition physique, de la vérification et de la contention IO externe.

## Mutations des observateurs pendant une publication groupée {#observer-changes-during-batch-publication}

Le CAS groupé conserve les octets originaux bornés des références pendant vérification et progression. Après le dernier observateur, il vérifie que le fichier reste identique. Insertion, retrait ou remplacement par un callback fait rendre false sans écraser sa modification, même pour une référence hors du groupe. Refaire la lecture avant nouvel essai. Une réécriture canonique identique peut continuer ; l’annulation reste vérifiée avant publication.

Ce contrôle est une lecture/comparaison linéaire d’au plus 16 Mio, sans second DOM ni changement de format. Il protège les mutations par callback, pas une altération arbitraire du système de fichiers ou des accès concurrents généraux au même handle. Le verrou exclusif et la sérialisation restent requis.
