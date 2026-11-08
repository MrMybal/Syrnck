---
id: native-blame
title: Attribution native des lignes
description: Blame vérifié à travers les merges et renommages exacts.
section: Versionnage
order: 251
parent: native-history
sourceRevision: 2
---

## Attribuer les lignes {#usage}

`native blame REVISION PATH` attribue chaque ligne d’un fichier texte UTF-8 commité
à la révision et au chemin qui l’ont introduite. Il reconstruit et vérifie les
octets stockés sans lire le workspace, modifier les références ou importer des
objets. L’entrée C++ est `native_blame` dans `syrnck/history.hpp`.

```text
syrnck native blame HEAD Source/Module.cpp
syrnck --lang fr native blame tags/release Source/Module.cpp --progress-json
syrnck native blame HEAD Source/Module.cpp --first-parent --no-follow
```

Le tableau JSON `lines` conserve numéro final, `revision` et `path` d’origine,
`source_line` à partir de 1 et `content` exact, y compris LF/CRLF ou absence de
saut de ligne final. Un fichier vide a zéro ligne. `commits` indexe les dates des
révisions d’origine et des aperçus de messages bornés. Les entrées v2 exposent
aussi `author`, `committer` et l’ID public `signing_key` (null sans signature).
La v1 ancienne n’a pas ces champs ; la commande ne les invente pas.
Voir [identité et confiance](doc:native-authorship).
Les clés JSON et identifiants restent en anglais.

## Merges et renommages {#lineage}

Par défaut, tous les parents ordonnés peuvent fournir des lignes inchangées. Le
premier parent correspondant prime, puis les lignes restantes sont comparées
aux parents suivants. Seules les lignes absentes de tous les parents proviennent
du merge lui-même. `--first-parent` limite le parcours à la chaîne des premiers
parents. Le moteur Myers borné effectue l’alignement ; des lignes répétées peuvent
admettre plusieurs alignements valides. Les espaces ne sont pas normalisés.

Les renommages à contenu exact et correspondance unique sont suivis même si le
découpage change, d’après hash et taille du fichier complet. Le mode exécutable ne
change pas l’origine des lignes. Plusieurs sources supprimées ou destinations
ajoutées possibles arrêtent ce suivi ; renommages modifiés et copies ne sont pas
déduits. `--no-follow` traite le nouveau nom comme un ajout. Un parent binaire ne
fournit pas d’origines textuelles.

## Budgets et erreurs {#limits}

Le parcours autorise par défaut 1000 révisions distinctes (1..10000), 64 Mio de
métadonnées décodées de révisions/manifestes et de fichiers reconstruits cumulés
(1..256 Mio), ainsi que les [budgets du moteur textuel](doc:text-diff). Ces budgets
d’entrée/calcul ne bornent pas toutes les allocations du processus. Le contenu
reconstruit est mis en cache par manifeste pour cette seule requête. Les racines
de rétention normales doivent toujours protéger l’historique consulté.

```text
syrnck native blame HEAD Source/Module.cpp --max-revisions 2000 --max-input-bytes 134217728
syrnck native blame HEAD Source/Module.cpp --max-file-bytes 2097152 --max-lines 60000 --max-work 33554432 --max-output-bytes 8388608
```

`status: complete` termine avec le code 0. Une entrée binaire rend `status: binary`,
raison `non_text`, code 1. Un budget de révisions/entrée/fichier/lignes/calcul/
trace/sortie épuisé rend `status: limited`, une raison et le code 1. Les rapports
incomplets ont `lines` et `commits` vides ; une attribution partielle n’est jamais
présentée comme complète. Chemin absent, objets corrompus/manquants, arguments
invalides et annulation font échouer la commande sans rapport partiel. Progression
et diagnostics suivent la langue choisie pour l’interface.
