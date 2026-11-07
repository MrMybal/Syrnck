---
id: text-patches
title: Patches unifiés stricts
description: Guide : Patches unifiés stricts.
section: Versionnage
order: 570
parent: native-history
sourceRevision: 1
---

# Patches textuels unifiés stricts {#strict-unified-text-patches}

apply_text_patch(source,patch,path,limits,operation) rend TextPatchResult avec contenu complet et comptes hunks/ajouts/retraits sans écrire fichiers, objets ou références. Une entrée invalide lance une erreur, sans résultat partiel et sans modifier les entrées.

## Format accepté {#accepted-format}

Un fichier UTF-8 existant par patch, labels exacts. Labels identiques sans préfixe et terminateur TAB final Git acceptés. Préambule exact diff --git a/PATH b/PATH et ligne index OLD..NEW avec 100644/100755 optionnelle possibles. Les hashes index sont des indications de format, pas des identités Git vérifiées : octets source/contexte fixent l’application. Compte de hunk omis si un ; texte de section après @@ accepté. Positions/comptes source et résultat, insertions de compte zéro comprises, doivent correspondre. Toute ligne contexte/retrait doit être identique à la position exacte, sans recherche d’offset, fuzz, correction d’espace ou inversion devinée.

Labels UTF-8 littéraux, métadonnées LF/CRLF. Hors sous-ensemble : labels cités/échappés, dates, renommages, modes, présence de fichiers, /dev/null, binaire, diff combiné/de mots, multi-fichier et wrappers mail/commit. Second fichier ou suffixe non géré refusé ; patch vide ou sans ajout/retrait aussi. Chemin portable validé, jamais ouvert. Le mode index ne change pas le mode exécutable source.

```text
--- a/Source/Player.cpp
+++ b/Source/Player.cpp
@@ -1 +1 @@
-old line
+new line
```


## Octets exacts et limites {#exact-bytes-and-bounds}

Les lignes de corps gardent contenu et LF/CRLF. Le marqueur standard de newline finale manquante s’applique à la ligne précédente, sans répétition ni orphelin. Le corps du patch doit lui-même se terminer par LF, avec le marqueur pour l’absence de LF de contenu ; ligne tronquée, ligne vide non terminée ou concaténation après une ligne non terminée refusées. Aucune normalisation.

Classification UTF-8/contrôles identique au [diff](doc:text-diff). max_file_bytes borne indépendamment source/résultat (1 Mio, plafond 4) ; max_output_bytes borne le patch entrant (4 Mio, plafond 8) ; max_lines borne chaque fichier (50000, plafond 100000). max_work facture octets de chaque ligne de patch +1, octets source comparés +1 et octets inchangés copiés +1 ; scans de validation aussi bornés par tailles. Positions/comptes et nombre de hunks au plus 100000. Context validé mais sans modifier un patch externe.

Mémoire : vues de lignes source et résultat bornés, sans recherche de distance ou trace de diff. Stop pendant validation/parcours/sortie ; observateur planning synchrone peut lancer une erreur, jamais de succès partiel.

## Préparer un patch dans l’index natif {#prepare-an-external-patch-in-the-native-index}

plan_stage_patch/stage_patch utilisent le fichier figé complet de staging/BRANCH, sinon heads/BRANCH. Pas de dossier ou lecture de travail ; sale, absent, ignoré ou devenu dossier sans effet. Le fichier source doit exister dans le snapshot ; une branche initiale est possible avec index initial contenant ce fichier.

Le plan donne operation stage-patch, branche/chemin/tête/index/base, révision source et métadonnées vérifiées, patch_hash BLAKE3 brut, taille du patch et comptes/taille/hash du résultat. Il ne réaffiche pas le patch : examiner l’original. plan_id est l’ID chunk du JSON canonique avant son ajout, sans objet plan. Options, patch, source et résultat sont liés.

L’application recalcule l’ID exact avant ingestion. Format, source absente/trop grande, binaire, contexte/plage/budget invalides échouent sans écriture. Un aperçu valide peut encore être bloqué par checkout, index périmé, résultat identique ou archive contradictoire. Il vérifie seulement le fichier ; publication vérifie candidat, ancienne préparation et branche, donc une autre donnée corrompue peut bloquer ensuite.

Le candidat garde toute la table figée, change le seul manifeste et conserve mode/voisins. Ingestion CDC et préférences habituelles, puis CAS tête/index/HEAD symbolique/archive publiant staging et archive/stage-index/OLD_INDEX_ID. Archive exacte réutilisable ; toute l’ancienne préparation/base reste reconstructible après GC et commit.

Arrêt/observateur/fermeture en échec avant publication gardent l’état ancien, objets complets inaccessibles possibles. Aucun journal workspace ni callback après publication. Rapport complet 16 Mio indépendamment du patch. Garder les octets empruntés stables pendant chaque appel.

## Parcours CLI {#cli-workflow}

Git --output écrit directement le patch sans transcodage shell. Choisir un seul fichier existant et la bonne source. Les hashes Git index n’autorisent aucun objet Syrnck. Les patches de modification générés par native diff-text --unified sont aussi acceptés, avec chemins Unicode/espaces, LF/CRLF, fichier existant vide, contenu vidé et plusieurs hunks.

Les deux commandes exigent --patch FILE et acceptent --branch, limites, progression et arrêt, mais pas --directory : application sur octets stockés. Patch lu une fois dans son budget avant plan/application ; changement lors d’une invocation ultérieure impose nouvel aperçu. Revoir patch et hashes source/résultat avant appliquer.

JSON identique en anglais/français. Prêt : 0 ; bloqué : 1 avec JSON ; patch/options/application invalides : non-zéro sans résultat. Succès : state applied, index précédent/nouveau, manifeste/hash résultat et ancien ID de plan conservé. Progression stderr ; voisins/modes préparés conservés au commit staged.

```text
git -c core.quotepath=false diff --no-ext-diff --no-textconv --output=change.patch -- Source/Player.cpp
syrnck native add-patch-plan Source/Player.cpp --patch change.patch
syrnck native add-patch Source/Player.cpp --patch change.patch --expect-plan PLAN_ID
syrnck native diff-text --staged Source/Player.cpp --unified
syrnck native commit --staged -m "External text changes"
```
