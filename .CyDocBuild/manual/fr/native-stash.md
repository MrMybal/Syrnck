---
id: native-stash
title: Stash natif
description: Guide : Stash natif.
section: Versionnage
order: 390
parent: native-history
sourceRevision: 2
---

# Stash natif {#native-stash}

Stash capture fichiers de travail complets et index figé avant nettoyage du workspace sélectionné ; l’application ultérieure restaure ce travail sans avancer la branche. API syrnck/stash.hpp sans dépendance réseau/CLI.

Utiliser l’ID canonique de 64 caractères rendu par push/list, sans alias positionnel mutable. --directory choisit le workspace principal ou le dossier lié enregistré partageant le Store ; --branch doit identifier la cible nommée ou détachée sélectionnée. Terminer d’abord toute récupération checkout/switch/reset/stash.

```text
syrnck native stash push -m "WIP: lighting"
syrnck native stash push --include-untracked -m "WIP: new assets"
syrnck native stash list --limit 100
syrnck native stash show STASH_ID --verify
syrnck native stash apply-plan STASH_ID --index --verify
syrnck native stash pop STASH_ID --index --expect-plan PLAN_ID
```


## Sauvegarde et portée {#save-and-scope}

La sauvegarde inclut chemins suivis par tête/index, indépendamment des ignores, et non-suivis ordinaires obstructifs aux transitions. --include-untracked ajoute les fichiers ordinaires sélectionnés par le scanner natif avec ignores ; ignorés sans rapport restent sur place. Liens/reparse, spéciaux, entrées Store/Git et noms ambigus sont refusés s’ils obstruent ; le scanner optionnel refuse aussi les liens sélectionnés. Profondeur 128.

Le snapshot de travail a pour parents tête et index éventuel. Ses fichiers gardent les octets du workspace ; son parent index garde les préparations même différentes. Les suivis manquants restent absents. Le nettoyage vise la tête courante. Les dossiers vides ne sont pas versionnés, mais le journal de reset garde ceux nécessaires à abort.

Métadonnées stash, génération monotone et journal hard-reset v5/pins sont publiés atomiquement avant tout déplacement. Une erreur antérieure peut laisser des objets complets non référencés ; ensuite stash conservé et nettoyage récupérable. Abort restaure workspace/index tout en gardant le stash comme copie supplémentaire. État propre sans index : stash null sans écriture.

List page du plus récent avec --before GENERATION, --limit N ; validation de métadonnées/références de travail sans tous les payloads. Aperçu de message 256 octets UTF-8, show rend le complet et --verify sa fermeture. JSON, IDs, messages utilisateur et noms d’opération restent stables entre langues.

## Réapplication, conflits et index {#reapply-conflicts-and-index}

apply-plan lit sans écrire ; application exige --expect-plan exact et recalcule avant candidats. Suivis sales, index existant, checkout actif et non-suivis obstructifs bloquent pour ne pas écraser un travail local.

Le travail sauvegardé fusionne contre tête d’origine et tête sélectionnée actuelle. --index fusionne séparément les préparations contre le parent propre de l’ancien index, même périmé, pour garder les changements de tête ultérieurs. Le nouvel index a pour base la tête courante. Sans --index, les préparations appartiennent au résultat de travail sans nouvel index figé.

Tout conflit bloque l’ensemble. --text-merge active le moteur borné ; binaire demande un choix explicite. Deux contextes working.resolution_context et index.resolution_context acceptent current/incoming/base/remove/file avec manifeste vérifié. Opérations stash et stash-index et IDs head/source/base/parent lient les choix. Le texte partage limites agrégées de tentatives/octets entre les deux résultats ; garder les mêmes options à l’application.

Journal checkout v6 distingue tête publiée et cible de travail. La tête ne bouge pas ; publication finale installe index éventuel et, pour pop, archive/retire le stash dans le même CAS que le reçu engagé. Apply garde le stash actif. La révision de travail conservée protège stash original et nouvel index après GC.

Avant publication finale, abort restaure le workspace propre précédent et laisse le stash actif. Après, récupération du nettoyage. Fichiers de travail/préparation/rollback en clair selon les limites checkout habituelles ; ouverture sans reprise implicite.

```text
syrnck native stash apply-plan STASH_ID --index --text-merge
syrnck native stash apply-plan STASH_ID --index --resolutions work.json --index-resolutions index.json
syrnck native stash apply STASH_ID --index --resolutions work.json --index-resolutions index.json --expect-plan PLAN_ID
```


```text
syrnck native checkout-status
syrnck native checkout-recover
syrnck native checkout-recover --abort
```


## Workspaces partiels {#partial-workspaces}

Stash accepte les [workspaces partiels](doc:native-workspaces) statiques, avec
HEAD détaché ou sélection explicitement vide. La sauvegarde capture l’arbre
physique sélectionné et les fichiers ordinaires faisant obstacle à son nettoyage.
--include-untracked suit cette même sélection et les règles d’exclusion. Les
fichiers locaux exclus sans rapport restent sur place. Tête/index anciens restent
parents complets, avec toutes les entrées cachées et leurs blocs.

Les descripteurs partiels utilisent v3 avec tableau canonique `materialization`
intégré. Les vues complètes nommées/détachées gardent v1/v2. Apply exige des
définitions sauvegardée/actuelle strictement identiques ; passage complet/partiel
ou changement de sélection refusé avant écriture des candidats. Le plan partiel
v2 lie définition et racine de l’objet de politique.

Le rejeu de travail compare base/tête projetées à l’arbre physique sauvegardé.
Le rejeu d’index reste complet : --index restaure les préparations cachées tout
en conservant les changements cachés indépendants d’une tête plus récente. Sans
--index, les données préparées cachées restent dans le parent index sauvegardé,
sans apparaître hors sélection sur disque.

Save utilise le journal v9 layout 5 ; apply/pop le layout 6 avec définitions
figées identiques. Le layout 6 projette la source logique et restaure le candidat
physique complet. Un fichier sauvegardé `Content` peut ainsi remplacer le chemin
sélectionné `Content/A/leaf`, sans introduire le `Content/B` logique exclu dans
ce namespace physique. Abort restaure la vue projetée propre précédente ; la
publication finale combine index complet et archivage du pop, avec HEAD logique
inchangé. GC et sauvegarde portable conservent descripteur, travail physique et
index/historique complets.

## Drop, restauration et rétention {#drop-restore-and-retention}

Drop exige l’ID d’enregistrement immuable attendu et déplace références actives de métadonnées/travail vers archives. Pop protège de la même façon ; aucun objet supprimé. Restore conserve la génération originale et exige l’ID actif absent. Archives et racines de travail survivent à sauvegarde/restauration et GC.

Les racines de travail peuvent être [retirées avec sauvegarde](doc:archive-retention). Un stash retiré ne se restaure plus depuis le Store courant sans réimporter protection/données de sa sauvegarde. D’autres archives/reçus checkout peuvent encore garder des blocs partagés.

```text
syrnck native stash drop STASH_ID --expect-record RECORD_ID
syrnck native stash archive STASH_ID --verify
syrnck native stash restore STASH_ID
syrnck native archives --kind stash --verify
```


## Bornes et vérification {#bounds-and-verification}

Au plus 4096 stashes actifs et 1000 lignes par page ; message 1 Mio. Révisions, inventaires et rapports gardent 16 Mio. Les octets passent en flux par ingestion configurée, mais inventaire et hashing restent nécessaires, distincts du watcher/index de travail.

Sous Windows à limite canonique de chemin ancienne, l’OS peut refuser un chemin avant profondeur 128 ; la prise en charge complète des longs chemins reste un travail de compatibilité distinct.
