---
id: native-checkout
title: Checkout récupérable
description: Guide : Checkout récupérable.
section: Versionnage
order: 230
parent: native-history
sourceRevision: 2
---

# Checkout natif récupérable {#recoverable-native-checkout}

`native checkout` restaure une révision exacte dans un workspace existant et déplace la branche sélectionnée. Sa tête précédente est archivée avant toute libération de rétention. Il applique le [preflight du workspace](doc:native-workspace).

La branche par défaut est la [courante](doc:native-branches), implicitement `main` dans les anciens Stores ; répertoire `.`. `--branch feature` déplace cette branche en prenant son snapshot comme base. `native switch BRANCH` change la sélection en conservant les deux têtes. Le Store doit être strictement à l’intérieur de la racine canonique.

Le CLI résout les [sélecteurs](doc:native-tags). C++ peut utiliser `resolve_native_revision()` ; l’API checkout prend toujours des IDs exacts.

```text
syrnck native checkout-plan REVISION --branch main --verify
syrnck native checkout REVISION --branch main --directory . --progress
syrnck native checkout-status
syrnck native checkout-recover --progress
syrnck native checkout-recover --abort --progress
syrnck native checkout-status --id TRANSACTION_ID
```


## Démarrer et publier {#starting-and-publishing}

Tout fichier suivi doit correspondre à sa base par hash complet et métadonnées exécutables prises en charge. Un index, même propre, bloque. Collisions ignorées/non suivies, liens/jonctions, chemins protégés et alias portables ambigus sont refusés. Casse et transitions fichier/dossier propres utilisent l’exécuteur de namespace. Aucun `--force`.

Un changement répète le preflight et vérifie source, cible, parents et hashes complets avant le journal. Un plan métadonnées seul ne suffit pas. Cible égale à la tête et workspace propre : `state: unchanged`, aucune transaction. Les autres fichiers non suivis/ignorés restent intacts.

Chaque cible est reconstruite et vérifiée en slot privé, puis la source revérifiée est déplacée vers une sauvegarde et la cible installée par déplacement exclusif sans écrasement. Une suppression garde l’original ; un changement de représentation seul ne déplace rien ; un changement de mode prépare un fichier complet. Unix applique les bits exécutables ; Windows conserve les métadonnées sans les appliquer ni signaler un changement local.

Après le dernier callback public, les fichiers désirés sont vérifiés. Une transaction compare-and-swap déplace la branche, valide le journal, archive la source et retient cible/reçu. Puis nettoyage vérifié et retrait des racines actives. Succès : `state: committed`, `pending: false`, `id`, `archive` ou null si branche non née.

L’atomicité concerne les références, pas les déplacements séquentiels visibles. Arrêter les écrivains externes : aucun snapshot atomique, verrou du workspace ou défense contre remplacement hostile. Une erreur IO/annulation après création du journal demande une récupération explicite.

## Inspecter, reprendre et abandonner {#inspecting-resuming-and-aborting}

Sans opération active, `checkout-status` donne `pending: false`. Sinon : identité immuable, branche, source/cible, racine canonique, état, `data_directory` et chemins/slots numériques. `--id` inspecte l’actif ou un reçu terminé. Les champs/états JSON restent anglais.

États et récupération : `preparing` termine la reconstruction puis revalide/applique ; abort jette les préparations. `applying` vérifie et termine ; abort restaure en ordre inverse. `evacuating` sauvegarde les sources/retire les dossiers requis ; `installing` installe puis publie ; abort désinstalle les cibles avant restauration. `uninstalling`, `restoring`, `aborting`, `discarding` poursuivent leur phase durable, quel que soit le nouvel appel. `aborted` ne fait que nettoyer. `committed` ne fait que nettoyer et refuse abort.

La récupération utilise exactement branche/répertoire enregistrés, sans override. Ouvrir le Store ne touche pas les fichiers ; copier/déplacer un journal n’est pas supporté. Une récupération terminée est idempotente. Si un fichier public/privé a changé, les octets sont préservés et l’étape risquée refusée : inspecter et sauvegarder les edits avant reprise. Une source modifiée juste avant sa sauvegarde est restaurée telle quelle. Le marqueur `aborted` précède le nettoyage pour ne pas confondre un fichier ultérieur. Des parents vides nouveaux peuvent rester ; aucune suppression récursive.

Pendant l’opération, GC même simulé, commit, add, reset, commit figé et détachement sont bloqués. Pack et consultation restent possibles. Les opérations génériques de références ne contournent pas la récupération : leurs mutations peuvent la bloquer. Ne pas éditer/répliquer les racines actives.

## Reprise contrôlée par l’agent résident {#owned-recovery}

L’[agent résident bidirectionnel](doc:resident-agent) marque ses checkouts entrants
et ceux des fusions avec un propriétaire explicite. Le journal v11 fige propriétaire,
contexte et les deux définitions de matérialisation, avec layout 1 ou 3 pour
l’application ordinaire ou les transitions de namespace. Le propriétaire participe
à l’ID immuable. `resident-sync-v1` est établi avant observations persistantes ou
journaux avec propriétaire ; les anciens lecteurs refusent le Store au lieu
d’ignorer ce nouvel état. Les encodages sans propriétaire ne changent pas.

```cpp
store.checkout_directory_owned(project, "main", target, owner_id, operation);
store.recover_checkout_owned(project, "main", owner_id, operation);
```

La reprise vérifie propriétaire, contexte sélectionné, workspace physique actuel
et cible HEAD avant toute application. Cet ID sert à la coordination, pas à
l’authentification face à un écrivain local autorisé du Store. L’agent exige aussi
que son observation distante conservée soit ancêtre de la cible préparée. Une
autre adresse, un autre workspace ou une session de publication seule restent bloqués.

Une transaction active de l’agent peut finir hors ligne sans clé privée : la
cible entrante ou de fusion signée est déjà figée et retenue. La reprise ne
recompose ni ne signe de révision. Une nouvelle opération signée exige toujours
l’identité de signature. Les transactions manuelles ou étrangères demandent
`checkout-recover` explicite ; ouvrir le Store seul ne reprend rien.
Les reçus terminés restent consultables et retenus suivant les règles existantes.
Les sauvegardes conservent exigences de lecteur et révisions complètes, sans
activer un ancien checkout physique dans un dossier restauré.

## Journal, rétention et données privées {#journal-retention-and-private-data}

Le journal immuable est un chunk ordinaire compressible/chiffrable/empaquetable. Identité : version, nonce, racine canonique, branche, source/cible. Switch v2 ajoute destination et HEAD. Les namespaces utilisent v3/v4 et `namespace_transition: true`, avec lecture v1/v2 conservée. Le [hard reset](doc:native-hard-reset) utilise v5 pour distinguer snapshot de travail, tête/index originaux et profil de répertoire. Chaque phase publie un nouveau journal.

Racines : `checkouts/active` (journal unique) ; `checkouts/pending/ID/target` et `/source` (fermetures jusqu’au nettoyage) ; `archive/checkout/ID` (ancienne tête) ; `checkouts/completed/ID` (cible publiée) ; `checkouts/receipts/ID` (journal validé consultable).

Les racines actives partent après nettoyage ; archives, cibles et reçus restent retenus sans purge automatique. Supprimer une branche ne supprime pas cet historique. Retirer toutes les racines puis GC destructif rend ces versions irrécupérables. Aucun reçu permanent pour abort.

Les slots `STORE/checkouts/ID/{new,old}/N.bin` contiennent du **clair même avec Store chiffré**. Les cibles et sauvegardes ne sont pas protégées par le chiffrement objet. Unix utilise des répertoires privés propriétaire ; Windows hérite des ACL du Store. Utiliser le chiffrement du système de fichiers pour ces octets. Prévoir la place des cibles ; les sauvegardes déplacent les originaux. Tous les chemins doivent supporter les déplacements exclusifs sur le même système de fichiers ; sinon journal conservé.

Le nettoyage vérifie noms, types et hashes, préserve tout contenu inattendu et ne retire que les dossiers vides. Windows peut réessayer leur retrait non récursif environ 500 ms. Les autres erreurs gardent le journal pour reprise.

## C++, progression et limites {#c-progress-and-limits}

Callbacks synchrones sans réentrée Store. Scan/plan/vérification gardent leurs événements ; préparation reconstruit par fichier, application/rollback signalent `exporting` avec octets logiques changés et vérifications. Les compteurs peuvent repartir entre phases/reprises. Le dernier observer avant publication est `publishing`, chemin `checkout`. Avant ce point, annulation/erreur préserve la branche mais peut laisser workspace partiel et journal. Après publication, nettoyage sans observer/annulation : une erreur peut donc accompagner une branche déjà validée. Inspecter et reprendre.

Tampon partagé 1 Mio, tables, caches de manifestes, objets 16 Mio et inventaire borné s’appliquent. Preflight/final hashes et fermetures restent complets ; fichiers changés reconstruits entièrement et sauvegardes hashées avant nettoyage. Pas de patch en place ni matérialisation parallèle ; le cache de travail consultatif ne remplace pas ces validations. L’ABI C et Go n’exposent pas encore checkout.

```cpp
auto result = store.checkout_directory(project, "main", target_id, operation);
auto pending = store.checkout_status();
auto resumed = store.recover_checkout(false, operation);
auto aborted = store.recover_checkout(true, operation);
auto receipt = store.checkout_status(transaction_id);
```


## Transitions de namespace récupérables {#recoverable-namespace-transitions}

Les fichiers suivis propres peuvent changer de casse, parents inclus, ou passer fichier/dossier : `Source/Code.cpp` vers `source/code.cpp`, fichier `Content` vers `Content/Map.uasset`, arbre entièrement suivi vers fichier `Content`. Les autres renommages sont suppressions/ajouts ; le suivi historique exact est [distinct](doc:native-file-history).

Le plan contrôle casse portable exacte et propreté complète, puis indique `namespace_transition` et `remove_directories`. Chaque dossier affecté doit contenir uniquement fichiers sources et parents connus. Descendants ignorés/non suivis, dossiers vides inconnus, liens et stockage protégé bloquent avant journal. Des parents portables de casses incompatibles dans un snapshot sont refusés.

Toutes les cibles sont préparées, puis **toutes les sources changées** évacuées avant installation. Seuls les dossiers dérivés du snapshot nécessaires sont retirés, du plus profond, et seulement vides. `installing` est persisté avant parents et déplacements. Hashes finaux, namespace et publication atomique conservent les garanties.

Abort remet les cibles dans leurs slots, retire les seuls dossiers cibles vides requis, persiste `restoring`, puis rétablit sources et orthographe. Des phases durables distinctes évitent de confondre originaux restaurés et cibles. Les edits ou nouvelles obstructions sont préservés et bloquent la récupération. Aucune récupération automatique à l’ouverture.

Switch, merge, replay et rebase linéaire partagent cet exécuteur. Switch préserve les têtes et publie HEAD avec le journal. Aucun nouveau format d’objet/pointeur/révision/réseau ; les vieux moteurs doivent refuser les versions de journal inconnues. V6 prend en charge l’[application de stash](doc:native-stash) sans déplacer la tête, avec index et pop publiés ensemble. Le [retrait des reçus terminés](doc:checkout-retention) exige une sauvegarde indépendante ; leur consultation historique n’exige pas l’ancien HEAD ou snapshot source volontairement retiré, contrairement à la récupération active.
