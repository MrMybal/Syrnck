---
id: native-bisect
title: Recherche par bissection
description: Guide : Recherche par bissection.
section: Versionnage
order: 210
parent: native-history
sourceRevision: 2
---

# Bissection persistante sans checkout {#persistent-native-bisection-without-checkout}

`syrnck::NativeBisect` réduit une plage connue bonne/mauvaise dans le DAG de révisions, fusions comprises. Son état persiste et chaque résultat exige l’ID d’état attendu. Ni branche, index ni workspace ne bougent ; tester un candidat est une action explicite de l’appelant.

Les bornes doivent être distinctes, la bonne accessible depuis la mauvaise initiale. Le CLI accepte les sélecteurs natifs. Le JSON rend `session`, `state`, `status`, `candidate_count`, `next_revision`, `first_bad`, `good`, `skipped`, `bad`. Marquer exactement le candidat suggéré avec le state du dernier rapport ; une autre mise à jour invalide l’ancien jeton. Une identité de session aléatoire empêche sa réutilisation lors d’une nouvelle recherche de la même plage.

Les révisions bonnes excluent tous leurs ancêtres ; les candidats restants sont ancêtres de la mauvaise courante. Pour chaque candidat éligible, le moteur calcule les partitions restantes selon bon/mauvais et choisit la plus équilibrée, départagée par ID. Les bitsets d’ascendance ne comptent pas deux fois les ancêtres partagés des fusions. La mauvaise courante n’est pas reproposée.

`status: found` identifie un premier mauvais. Un candidat sauté/non compilable demeure possible ; sans test non sauté restant, le résultat devient ambiguous, first_bad null et possible_first_bad liste les possibilités, sans choisir arbitrairement. Comme toute recherche de régression, utiliser un même critère dont le défaut persiste chez les descendants ; des résultats contradictoires ou critères changeants ne prouvent pas une introduction fiable.

```text
syrnck native bisect-start GOOD_REVISION BAD_REVISION
syrnck native bisect-status
syrnck native export CANDIDATE_REVISION /new/candidate-directory
syrnck native bisect-good CANDIDATE_REVISION --expect-state STATE_ID
syrnck native bisect-bad CANDIDATE_REVISION --expect-state STATE_ID
syrnck native bisect-skip CANDIDATE_REVISION --expect-state STATE_ID
syrnck native bisect-reset --expect-state STATE_ID
```


## État, rétention et récupération {#state-retention-and-recovery}

Une recherche active par contexte de workspace. Le `bisect/active` principal pointe un descripteur chunk canonique strict v1 ; `bisect/pin` conserve la mauvaise initiale, ses ancêtres et fichiers. Le démarrage vérifie les fichiers et publie état/pin ensemble. Déplacer ou retirer une branche ne rend donc pas ces versions collectables.

Chaque marquage publie un nouvel état immuable par transaction attendue ; les anciens états sans référence sont collectables, sans archive croissante à chaque étape. Annulation/coupure avant publication gardent l’ancien état et peuvent laisser des métadonnées orphelines complètes. Un nouveau processus retrouve la même recherche par bisect-status, sans cache nécessaire. Descripteur corrompu, pin absent/incohérent, résultat hors historique et métadonnées contradictoires sont refusés.

Reset termine une recherche, achevée ou non, et publie ensemble la protection `archive/bisect/INITIAL_BAD` et le reçu final `bisect/receipts/SESSION`. Les anciens fichiers restent reconstructibles après GC ; `native archives --kind bisect` les inspecte et le [retrait avec sauvegarde](doc:archive-retention) permet leur nettoyage. Une nouvelle recherche possède une session indépendante.

Bornes : 100000 révisions, 1000000 liens de parents, budget estimé de métadonnées de graphe 64 Mio, au plus 4096 candidats non résolus. Les bitsets denses utilisent au plus 2 Mio ; conteneurs/caches/allocateur s’ajoutent. Réduire la plage avec une bonne borne plus récente si nécessaire. Aucun index persistant de graphe n’évite encore les relectures de statut.

Les contextes liés gardent leurs propres état actif, pin et reçus, sélectionnés
avec `--workspace NOM`. Plusieurs workspaces peuvent garder des recherches
indépendantes ; les mutations du Store restent sérialisées par son verrou.
Le [blame et l’historique des renommages exacts](doc:native-file-history) sont des
requêtes distinctes. Checkout bisect interactif, exécution automatique de commande,
sélection de prédicat tenant compte des renommages et signatures restent à réaliser.
