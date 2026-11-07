---
id: text-diff
title: Différences textuelles bornées
description: Guide : Différences textuelles bornées.
section: Versionnage
order: 550
parent: native-history
sourceRevision: 1
---

# Différences textuelles bornées {#bounded-text-differences}

compare_text(source_bytes, target_bytes, options, operation) calcule des changements de lignes déterministes sans écrire. Comparaison exacte des espaces, LF/CRLF et newline finale, avec UTF-8 requis. Encodage invalide, NUL ou contrôle autre que tabulation/LF/CR immédiatement avant LF donne status binary sans hunks ; aucun décodage automatique d’UTF-16 ou d’assets arbitraires.

Un résultat text contient equal, tailles et lignes source/cible, additions/deletions, hunks ordonnés. Chaque hunk donne plages start/count et lignes kind context/remove/add, text sans LF, newline. Le CR de CRLF reste dans text. Numérotation à partir de 1 ; plage vide indique le nombre de lignes précédentes. Fichier vide = zéro ligne, sans ligne vide inventée après LF final.

Le moteur retire préfixe/suffixe communs puis cherche un script minimal de lignes par Myers borné avec départage déterministe. Des répétitions peuvent s’aligner différemment d’un autre outil. Aucun diff de mots, renommage ou merge implicite.

Défauts/maxima : contexte 3/100 ; octets par entrée 1/4 Mio ; lignes 50000/100000 ; travail 16777216/268435456 unités ; sortie JSON 4/8 Mio. Trace fixe de 1048576 entiers, environ 4 Mio. Chaque diagonale/backtrack et comparaison de ligne de même longueur facture ses octets plus un. Scan, UTF-8, découpage et sortie sont aussi bornés. Vues de lignes, éditions, JSON, buffers et allocations s’ajoutent : pas de plafond RSS.

Dépassement : limited avec file_limit/line_limit/work_limit/trace_limit/output_limit, sans hunks/comptes partiels. equal est null si la taille empêche la comparaison ; sinon égalité d’octets même après limite ultérieure. Binaire sans comptes de lignes/éditions. Options invalides, annulation et exceptions propagées, jamais transformées en succès/limite. Callbacks sur appelant, non réentrants.

## Comparer un fichier entre révisions {#compare-a-native-file-between-revisions}

Les sélecteurs suivent HEAD/branche/tag. C++ : diff_revision_file, révision optionnelle null = snapshot vide. Chemin exact portable présent au moins d’un côté ; présence distincte d’un contenu vide.

Enveloppe : path, source_revision/target_revision, résumés nullable source/target avec manifeste/taille/hash/mode, equal, content_changed, executable_changed, manifest_changed, objects_verified et diff. Equal de l’enveloppe inclut présence/contenu/mode ; diff.equal ne compare que les octets. Un ajout de fichier vide peut donc être inégal dans l’enveloppe et égal dans diff. Découpage différent peut changer le manifeste sans changer les octets.

Les deux tailles sont contrôlées avant toute lecture de bloc. File_limit = métadonnées seulement, objects_verified false. Sinon les deux fichiers complets sont reconstruits/hashés même si binaire/égaux ou limite ultérieure. Donnée absente/corrompue échoue sans patch. Seul ce chemin est lu, pas les voisins/parents. Enveloppe totale 16 Mio.

JSON canonique par défaut, code 0 pour inspection réussie même binaire/limited. --unified émet a/ et b/, /dev/null si absent et marqueurs exacts de newline manquante ; binaire/limited refusés avant stdout, aucun hunk = sortie vide. Modes/représentations/présence vide nécessitent le JSON : le patch ne représente que le texte, pas toute une révision native.

Contexte 0..100 et options de taille/lignes/travail/sortie s’appliquent par invocation selon la table. Limites invalides avant inspection ; dépassement runtime décrit explicitement. Progression stderr et diagnostics bilingues.

```text
syrnck native diff-text SOURCE TARGET Source/Player.cpp
syrnck native diff-text HEAD release Source/Player.cpp --context 5 --unified
```


## Inspecter les changements figés {#inspect-frozen-staged-changes}

diff_staged_file compare chemin dans index et base d’origine. Sans index, tête contre elle-même ; index initial, source vide. Un index périmé garde sa base sans substituer la tête courante ni lire le disque. Les éditions ultérieures n’interviennent pas ; suppression préparée = cible absente.

L’enveloppe ajoute comparison staged, branch, base/head/index nullable et stale. Marqueur de branche, date et forme des parents de l’index sont contrôlés. Le CLI suit HEAD sauf --branch. Mêmes options, intégrité, JSON/unified et annulation que la comparaison de révisions. Aucune consommation/rebase/référence ou lecture de workspace. Chemin absent des deux snapshots = erreur même s’il existe maintenant sur disque. [Hunks](doc:native-hunks), [lignes](doc:native-lines) et [patches externes](doc:text-patches) offrent la préparation explicite séparément.

```text
syrnck native diff-text --staged Source/Player.cpp
syrnck native diff-text --staged Source/Player.cpp --branch feature --unified
```


## Comparer un fichier de travail {#compare-a-working-file}

Source par défaut = index figé de la branche choisie, sinon sa tête ; --branch remplace la branche. Un index périmé reste source avec sa base/tête décrites. --revision choisit plutôt une source commitée explicite et exclut --branch. --working exclut --staged. Branche initiale sans index = base vide ; suppressions et ajouts non suivis/vides explicites.

Seuls dossiers ancêtres sont énumérés pour orthographe exacte, collisions portables et ignores imbriqués, sans parcourir les sous-arbres sans rapport. Ignoré = erreur, suivi ou absent compris, jamais suppression silencieuse. Liens/reparse, alias de casse, obstructions de type et Store protégé refusés. Le Store est sous le workspace et HEAD doit correspondre à sa racine. Orthographe UTF-8 physique, normalisation comprise.

Inspection : profondeur 128 composants, budget de 16 Mio pour noms énumérés et ignores par passage ; au plus 16384 règles de 4096 octets. Sémantique des globs identique au staging, limites propres à ce lecteur ciblé.

Tailles source/travail contrôlées avant payload. Ensuite source complète reconstruite/vérifiée, travail lu par 64 Kio avec détection croissance/réduction. Namespace/taille/date/mode disponible revérifiés autour de la lecture ; mutation observée = refaire. Aucun snapshot atomique ni défense universelle contre falsification de dates ou échanges entre contrôles.

Enveloppe comparison working, directory canonique, source_revision, target_revision null. target.manifest et manifest_changed null ; hash calculé seulement si lu. workspace_verified et objects_verified indiquent contrôles terminés, tous deux false à limite de taille. Présence/taille peuvent prouver content_changed ; sinon contenu/égalité restent null si non comparés. Windows : target.executable null et executable_comparable false, mode ignoré pour égalité ; Unix compare le mode. La variante branche ajoute branch/head/base/index/stale. Aucun import, changement de fichier, index ou référence.

```text
syrnck native diff-text --working Source/Player.cpp --unified
syrnck native diff-text --working Source/Player.cpp --branch feature
syrnck native diff-text --working Source/Player.cpp --revision HEAD --directory .
```


```cpp
auto explicit_base = store.diff_working_file(project, revision_id, "Source/Player.cpp", limits, operation);
auto unstaged = store.diff_unstaged_file(project, "feature", "Source/Player.cpp", limits, operation);
```
