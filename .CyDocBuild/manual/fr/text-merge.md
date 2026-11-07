---
id: text-merge
title: Fusion textuelle à trois voies
description: Guide : Fusion textuelle à trois voies.
section: Versionnage
order: 560
parent: native-history
sourceRevision: 1
---

# Fusion textuelle à trois voies bornée {#bounded-three-way-text-merge}

merge_text(base,current,incoming,limits,operation) compare les deux côtés à une base et combine les éditions indépendantes. Un changement identique des deux côtés apparaît une fois. Politique prudente : chevauchement donnant des résultats différents reste conflit, insertion à la frontière d’un remplacement comprise. Pas de merge par caractère ou inférence de renommage.

## Résultat {#result}

Le JSON v1 donne tailles/lignes d’entrée, current_changes/incoming_changes (groupes d’éditions contigus contre base), status, reason, conflicts et content. Clean : chaîne UTF-8 exacte, même vide ; conflict, binary et limited : content null. Binary signifie UTF-8/contrôles invalides ; limited signale une borne d’entrée/résultat/recherche/trace/sortie.

Chaque conflit contient plages base/current/incoming start/count. Plage non vide à partir de 1 ; vide désigne la ligne précédente, zéro au début. Les coordonnées tiennent compte des insertions/retraits antérieurs et un groupe connecté de chevauchements donne un conflit. Seulement des coordonnées, sans marqueurs textuels.

LF, CRLF, mélanges acceptés et absence de LF finale sont conservés exactement, sans newline ou marqueur inventé. Une entrée binaire est refusée même si les trois sont égales. Une limite efface conflits, comptes et contenu plutôt que rendre une fusion partielle.

## Budgets et annulation {#budgets-and-cancellation}

TextDiffOptions : défaut 1 Mio par entrée/résultat, 50000 lignes, 16777216 unités de travail, 4 Mio JSON ; plafonds 4 Mio, 100000 lignes, 268435456 unités, 8 Mio. Context est validé sans affecter les décisions. Les deux recherches Myers et l’assemblage partagent le budget ; trace réutilisée plafonnée à environ 4 Mio par recherche. Tables de lignes, groupes et résultat bornés, sans plafond global de RAM. Le JSON du résultat propre, échappement compris, doit tenir.

Motifs : file_limit, line_limit, work_limit, trace_limit, output_limit, result_file_limit, result_line_limit. Une jonction après ligne non terminée est refusée par unterminated_line_join. Annulation/exception propagées ; aucune écriture de fichier ou objet.

## Fusionner un chemin stocké {#merge-one-stored-path}

merge_revision_file reçoit trois snapshots explicites et un chemin, sans choisir de base de graphe ; l’appelant peut prendre celle du plan natif. Enveloppe avec trois IDs, résumés de fichiers base/current/incoming nullable, ready, objects_verified, result_executable, blockers et merge. Seul clean est prêt et donne un mode final. Les modes fusionnent séparément : current inchangé prend incoming, sinon current est gardé. La représentation du manifeste n’altère pas l’égalité de texte.

Les trois chemins doivent exister. Absent : file_presence et merge null ; taille excessive contrôlée avant blocs : file_limit et merge null ; les deux ont objects_verified false. Sinon fichiers complets et hashes sont vérifiés, même si binaire/conflit/limite ultérieure rendent non prêt avec motif dans blockers. Donnée absente/corrompue lance une erreur, pas un résultat fusionné.

Audit seulement des chemins demandés, sans voisins/ancêtres, sans workspace ou publication. Enveloppe bornée à 16 Mio, rapport interne à max_output_bytes.

## CLI et résolution revue {#cli-and-reviewed-conflict-resolution}

Le CLI résout les sélecteurs natifs. JSON par défaut ; succès zéro seulement si ready. Bloqué : code 1 avec rapport complet. --text écrit uniquement les octets propres, sans newline finale ajoutée ; conflit, binaire, absence ou limite échouent sans contenu stdout. Texte/IDs/JSON indépendants de la langue, diagnostics bilingues. Les quatre limites textuelles et progression/annulation sont disponibles.

Cette commande est un aperçu explicite ; merge, cherry-pick/revert, rebase et rebase-index ont aussi leur composition opt-in. Pour un fichier personnalisé : garder le contexte exact du plan ; passer base/head/source au merge-text ; vérifier JSON/mode ; enregistrer --text hors du workspace avec un outil préservant les octets (d’anciens shells peuvent transcoder) ; importer via put et conserver sa racine ; ajouter le choix file/manifeste/mode au contexte ; replanifier --resolutions --verify puis appliquer avec même tête/document.

Les contrôles de publication, journal et archives protègent ensuite commit/parents. Les autres conflits bloquent toujours. Le preview seul ne lie ni ne publie une branche. Les cas racine/ajout/suppression gardent les règles de conflit de leur opération.

```text
syrnck native merge-text BASE CURRENT INCOMING Source/Player.cpp
syrnck native merge-text BASE HEAD feature Source/Player.cpp --text
```


## Préparation virtuelle {#virtual-file-preparation}

Le lot textuel interne prédit les manifestes en mémoire avec frontières CDC et encodage v1 de production. Les octets propres sont gardés jusqu’à l’application, sans écriture au plan. La matérialisation utilise le pipeline configuré et exige l’ID prédit avant usage par une révision. Une représentation existante est reprise si le contenu logique fusionné égale une entrée.

Au plus 64 conflits tentés et 16 Mio pour somme des entrées vérifiées/résultats propres, en supplément des limites par fichier. Hors budget, le conflit reste avec file_count_limit/total_bytes_limit, jamais une approximation. Les rapports exposent statut/plages/métadonnées cibles, pas les octets générés.
