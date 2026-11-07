---
id: native-rebase-todo
title: Rebase éditable et interactif
description: Guide : Rebase éditable et interactif.
section: Versionnage
order: 320
parent: native-history
sourceRevision: 1
---

# Rebase éditable et interactif {#editable-and-interactive-native-rebase}

Un rebase linéaire peut recevoir une séquence complète revue, par JSON ou invite de terminal. Les deux utilisent le même planificateur, résolutions, composition textuelle et checkout récupérable.

Garder le document hors des fichiers suivis, par exemple dans le Store, pour ne pas gêner le checkout. Le modèle contient IDs exacts head/base/onto et une étape par commit source. Déplacer les lignes pour réordonner ; conserver chaque source exactement une fois et utiliser drop explicitement. Champs manquants/inconnus, clés/sources dupliquées, commits étrangers, contexte périmé ou plage incomplète échouent avant création de candidat.

Remplacer les IDs illustratifs par les 64 caractères minuscules du modèle. Messages Unicode, multilignes et vides sont possibles. pick rejoue delta en gardant message/date ; drop ne rejoue rien mais archive l’histoire originale ; reword rejoue avec message obligatoire et date conservée ; squash replie le delta sur le résultat choisi précédent avec parent/date conservés et message source ajouté après deux newlines ; fixup replie en gardant aussi son message.

Seul reword accepte message. Squash/fixup exigent un résultat choisi antérieur, drops intermédiaires ignorés. Un fold remplace le candidat virtuel précédent sans parent/commit supplémentaire ; ses objets provisoires ne sont pas stockés. L’archive de l’ancienne tête protège les sources, pas les snapshots provisoires non publiés.

Chaque delta reste mesuré contre le vrai parent source original malgré réordre/drop : supprimer une dépendance peut créer un conflit, jamais remplacer silencieusement tout le snapshot source. --resolutions/--text-merge peuvent le résoudre. Le contexte inclut la tête virtuelle courante ; changer des actions antérieures périme les choix. Une source dropped ne peut avoir de résolution.

Une séquence explicite est honorée même si base = onto. Tout drop déplace la branche sur onto via le journal normal. Sans todo, l’ancien no-op même-base et ordre du plus ancien au plus récent restent disponibles.

```text
syrnck native rebase-todo OLD_BASE --onto upstream > todo.json
syrnck native rebase-plan OLD_BASE --onto upstream --todo todo.json --verify
syrnck native rebase OLD_BASE --onto upstream --todo todo.json --expect OLD_HEAD
```


```json
{
  "version": 1,
  "type": "native-rebase-todo",
  "head": "HEAD_ID",
  "base": "BASE_ID",
  "onto": "ONTO_ID",
  "steps": [
    {"source": "SECOND_ID", "action": "reword", "message": "New message"},
    {"source": "FIRST_ID", "action": "squash"},
    {"source": "THIRD_ID", "action": "fixup"},
    {"source": "FOURTH_ID", "action": "drop"}
  ]
}
```


## Saisie au terminal {#terminal-input}

L’invite affiche IDs et aperçus de messages échappés sur stderr. Entrer la séquence complète avec action et ID entier par ligne. Une ligne point confirme ; point seul choisit la séquence pick initiale. EOF avant confirmation échoue sans candidat ni changement. Ctrl+C demande l’annulation. Les entrées ne sont jamais du texte shell ; les actions restent anglaises. JSON pour messages multilignes.

--todo et --interactive sont exclusifs ; pas de progression JSON avec l’invite. rebase-plan --interactive examine sans appliquer. Résultat JSON seul sur stdout. Entrée redirigée UTF-8 ; console Windows lue en UTF-16 puis convertie sans changer codepage ou traiter les backslashes du message comme séparateurs de chemin.

```text
syrnck native rebase OLD_BASE --onto upstream --interactive
```


```text
reword SECOND_ID New message
squash FIRST_ID
fixup THIRD_ID
drop FOURTH_ID
.
```


## Bornes, récupération et portée {#limits-recovery-and-scope}

Les règles de plage linéaire, propreté et vérification complète s’appliquent. Todo JSON et saisie interactive agrégée : 16 Mio ; 100000 lignes ; 1 Mio par message explicite/combiné. Rapport séparément borné à 16 Mio, avec todo, action par étape, replaces des folds, result_commits, dropped_commits. completed_commits/replayed_commits comptent les deltas non dropped ; result_commits la chaîne finale après folds. Le modèle est aussi dans le plan ordinaire ; une plage non linéaire n’en a pas.

L’application vérifie les entrées, matérialise les commits survivants puis installe fichiers, branche et archive de l’ancienne tête avec le journal. Après interruption, suivre les [commandes de rebase](doc:native-rebase#apply-and-recover). Une préparation échouée/annulée peut laisser des objets valides non référencés.

Il s’agit de réécriture linéaire interactive. Préservation des fusions, lancement d’éditeur, étapes split/edit, association automatique fixup et exécution de tests ne sont pas implémentés par ces actions.
