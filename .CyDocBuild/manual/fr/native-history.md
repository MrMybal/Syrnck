---
id: native-history
title: Ascendance des révisions
description: Guide : Ascendance des révisions.
section: Versionnage
order: 300
sourceRevision: 1
---

# Ascendance des révisions natives {#native-revision-ancestry}

`native ancestry` parcourt le graphe complet accessible depuis deux snapshots et rend les deux directions d’ascendance ainsi que toutes les meilleures bases communes. Il ne choisit pas arbitrairement une base lorsqu’une fusion croisée en possède plusieurs.

Le JSON contient `source`, `target`, `source_is_ancestor`, `target_is_ancestor`, `related`, `merge_bases` triées, `visited_revisions` et `parent_links`. Une révision est son propre ancêtre. Des historiques sans lien rendent une liste vide et `related: false`, avec succès. Le CLI accepte les sélecteurs natifs usuels ; l’API C++ reçoit des IDs.

Le parcours itératif réalise un travail linéaire sur le graphe après décodage des métadonnées. Chaque révision distincte est chargée une fois. Tous les parents, y compris ceux des fusions, participent ; les doublons de parents comptent dans le budget de liens. Défauts : 100000 révisions et 1000000 liens. Réglages explicites : 1..1000000 révisions, 0..10000000 liens. Un dépassement échoue sans résultat partiel. La liste des bases reste conservativement sous 16 Mio JSON ; les révisions conservent aussi cette limite d’objet. La mémoire dépend des révisions et liens visités.

Seuls les objets révision sont lus. Manifestes et blocs ne sont pas nécessaires : cette requête ne prouve pas l’intégrité des fichiers. Toute révision accessible absente ou corrompue provoque une erreur, même si une réponse probable existait déjà. Aucun repli d’historique superficiel ni téléchargement implicite n’est effectué. Utiliser `fsck` ou une opération vérifiée pour les fichiers.

La requête ne change ni références, ni stockage, ni workspace. Progression de planification, annulation et diagnostics bilingues suivent le contrat d’opération ; les callbacks ne doivent pas réentrer dans le Store. L’exposition en C/Go reste à développer.

```text
syrnck native ancestry tags/v1 HEAD --progress-json
syrnck native ancestry heads/main heads/feature --max-revisions 100000 --max-links 1000000
```


```cpp
auto report = store.native_ancestry(left_id, right_id, syrnck::NativeGraphLimits{}, operation);
```
