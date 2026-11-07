---
id: native-tags
title: Tags et sélecteurs de révision
description: Guide : Tags et sélecteurs de révision.
section: Versionnage
order: 400
parent: native-history
sourceRevision: 1
---

# Tags natifs, sélecteurs et archives {#native-tags-revision-selectors-and-retained-archives}

Un tag nomme une révision commitée indépendamment d’une branche. Léger, il conserve sa cible ; avec un message, il ajoute une annotation UTF-8 et sa date. Ces opérations ne lisent ni ne modifient workspace, index ou HEAD.

La cible par défaut est HEAD commité. Modifications de travail et préparations locales ne sont pas incluses. Un HEAD initial vide exige une révision existante explicite. La création vérifie toute la fermeture cible, parents et hashes complets compris. Un nom existant est refusé même pour la même cible ; aucune option force/move n’est fournie.

Noms : lettres/chiffres ASCII, slash, point, tiret, underscore ; aucun composant vide, `.` ou `..`, aucun tiret initial ; au plus 187 octets pour tenir dans les deux namespaces. Messages : 65536 octets UTF-8 maximum ; un message vide explicite crée une annotation. L’OS peut limiter davantage la ligne de commande. Auteur de tag et signatures restent à développer.

```text
syrnck native tag release/v1
syrnck native tag release/v2 HEAD -m "Release notes"
syrnck native tags --limit 100
syrnck native tag-show release/v1 --verify
syrnck native diff tags/release/v1 HEAD
syrnck native checkout-plan tags/release/v1 --verify
```


## Rapports en lecture seule et pagination {#read-only-reports-and-pagination}

`native tags` rend `version`, `tags` triés, `has_more`, `next_after`. Limite 1..1000, défaut 100, et plafond de JSON sérialisé inférieur à 16 Mio, échappement inclus. Chaque tag expose `name`, `revision`, `annotation` (ID chunk ou null), `message`, `timestamp` ; les deux derniers sont null pour un tag léger. Dates Unix UTC, JSON anglais et messages utilisateur inchangés.

Le curseur `next_after` est exclusif et reste utilisable si le tag a été supprimé. Chaque appel utilise son snapshot de références, sans inventaire persistant ni calcul du total ; les mutations entre pages peuvent changer la vue. Une page peut contenir moins que sa limite. Le plafond de sortie ne borne pas la RAM : références, caches, annotation et buffers s’ajoutent. Les annotations affichées et racines orphelines sont contrôlées sans lire tous les arbres ni payloads.

`tag-show NAME` rend `version`, `tag`, `objects_verified`. `tag-archive ID` ajoute l’ID d’archive et `deleted_at`. `--verify` audite la fermeture complète de la cible. Par défaut, références/annotations/descripteurs sont inspectés sans preuve de disponibilité des blocs/parents. Annotations invalides, identités incohérentes et groupes incomplets ou avec membres inattendus sont refusés, sans réparation ni écriture.

`tag-archives [--after ID] [--limit N] [--verify]` liste les tags supprimés par ID d’archive. Champs : `version`, `archives`, `has_more`, `next_after`, `objects_verified` ; lignes `archive`, `tag`, `deleted_at`. Même défaut/plafond de lignes et de JSON. Le curseur exclusif survit au retrait de l’archive. Chaque page contrôle ses groupes complets et annotations ; `--verify` audite seulement les fermetures affichées. Aucune vue figée sur plusieurs requêtes.

## Suppression archivée et restauration {#archived-deletion-and-restoration}

La suppression exige `--expect` avec l’ID exact inspecté, pas un sélecteur mobile. Une transaction retire tag/annotation publics et publie descripteur, cible et annotation éventuelle de l’archive, dont l’ID est rendu. Un échec de vérification/attente ne supprime rien. L’archive conserve tout l’historique et ses blocs partagés après GC, même sans branche ni tag public.

Après perte de réponse, inspecter le tag et `tag-archives`, puis `tag-archive ID`. Les groupes portent `archive/tags/ID/record`.

La restauration vérifie l’archive et recrée nom, cible, message et date d’annotation ensemble. Un emplacement déjà occupé par un tag ou une annotation est refusé ; l’archive reste conservée. On peut recréer volontairement un tag à une autre révision sans perdre l’ancienne archive.

Le [retrait protégé par sauvegarde](doc:archive-retention) accepte le groupe entier `archive/tags/ID`. Sa recette et ses payloads restent dans la sauvegarde indépendante vérifiée. Pas de purge automatique. Les API administratives peuvent contourner ces racines : l’immutabilité n’est ni autorisation ni signature. Retirer toutes les protections puis collecter rend volontairement cette version indisponible. Le descripteur seul ne conserve pas sa cible : préserver tout le groupe lors d’une copie.

```text
syrnck native tag-show release/v1
syrnck native tag-delete release/v1 --expect EXACT_REVISION_ID
syrnck native tag-archives --verify
syrnck native tag-archive ARCHIVE_ID --verify
syrnck gc --delete
syrnck native tag-restore ARCHIVE_ID
```


## Sélecteurs de révisions {#native-revision-selectors}

Création de tags/branches, `status --revision`, diff de snapshots, checkout et export acceptent : ID exact de 64 caractères hexadécimaux minuscules (prioritaire sur un nom identique) ; HEAD de la branche symbolique commitée ; `heads/NAME` ; `tags/NAME` avec contrôle de l’annotation ; ou NAME non ambigu entre branche et tag.

Même si leurs cibles sont identiques, un nom partagé est ambigu : préciser le namespace. Un tag nommé HEAD ou comme un hash reste accessible par `tags/NAME`. La résolution vérifie les métadonnées de la révision seulement, pas ses parents, manifestes ou blocs ; la vérification complète reste distincte.

`switch NAME` sélectionne toujours une branche littérale. `checkout TAG` restaure la révision et déplace la branche sélectionnée en archivant sa tête ; il ne crée pas un HEAD détaché. Les méthodes C++ gardent leurs IDs exacts ; appeler `resolve_native_revision()` pour les sélecteurs.

## Stockage, annulation et API {#storage-cancellation-and-api}

`tags/NAME` cible une révision ; `tag-metadata/NAME` pointe l’annotation canonique facultative. `archive/tags/ID/record` porte le descripteur chunk d’ID identique ; `/target` la révision ; `/annotation` le chunk initial éventuel. Les schémas stricts lient nom, cible et annotation. L’archive ajoute un nonce et une date de suppression. Compression/chiffrement au repos habituels, aucun nouveau type d’objet, pointeur, ABI ou format physique.

Les transferts génériques ne négocient pas encore une publication atomique des groupes de tags annotés/archives. Politiques serveur de tags et réplication coordonnée restent à développer.

Toutes les commandes permettent progression et arrêt coopératif. Les mutations publient par CAS après fermeture complète. Avant publication, arrêt/exception de callback conservent les racines et peuvent laisser des chunks complets non référencés. Aucun callback ni contrôle d’arrêt après publication ; une coupure laisse l’ancien ou le nouvel ensemble complet. Les tags restent utilisables pendant un checkout en attente puisqu’ils ne changent ni branche ni racines de récupération. Callbacks non réentrants. Les opérations de tags ne sont pas encore exposées en C/Go.

```cpp
auto selected = store.resolve_native_revision("HEAD", operation);
auto created = store.create_native_tag("release/v1", selected, "Release notes", operation);
auto page = store.native_tags({}, 100, operation);
auto shown = store.native_tag("release/v1", true, operation);
auto removed = store.delete_native_tag("release/v1", selected, operation);
auto archived_page = store.native_tag_archives({}, 100, true, operation);
auto saved = store.native_tag_archive(removed["archive"].get<std::string>(), true, operation);
auto restored = store.restore_native_tag(removed["archive"].get<std::string>(), operation);
```
