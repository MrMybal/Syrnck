---
id: git-assets
title: Catalogues d’assets Git
description: Guide : Catalogues d’assets Git.
section: Git
order: 200
sourceRevision: 1
---

# Catalogues d’assets Git et bundles conservés {#git-asset-catalogs-and-retained-bundles}

Syrnck lit les pointeurs dans l’arbre Git commité, indépendamment du workspace sale, de l’index et des filtres. Git garde le code et son historique ordinaire ; une révision Syrnck déterministe décrit seulement les assets d’un commit. La [migration LFS](doc:git-lfs-migration) séparée vérifie le cache SHA-256 puis retient les blocs/catalogues avant publication d’une branche enfant.

catalog est en lecture seule sans Store ni identifiants. JSON : version, commit Git complet résolu, assets ordonnés avec path, blob, manifest, size logique, executable. Dépôts SHA-1/SHA-256 et tags annotés de commit sont gérés ; résolution source unique avant lecture de l’arbre.

prepare-assets utilise le Store commun git-common-dir/syrnck sauf --store. Initialiser avec git install. Chaque taille de pointeur et fermeture complète/hash de fichier est vérifié avant pin local git-assets/COMMIT. Échec sans perte de pins ; worktrees partagent le Store sous verrou exclusif.

Le bundle est une révision sans parent, timestamp zéro, message Syrnck Git assets v1 COMMIT et entrées d’assets, sans fichiers source et sans remplacer le commit Git. Même catalogue donne même ID. Chemins portables/collisions et 16 Mio s’appliquent. Le pin protège les versions indépendamment des références Git ; son retrait reste explicite. La [réconciliation de rétention](doc:git-retention) ajoute des pins par dépôt depuis références/reflogs/index ; elle ne les retire pas automatiquement.

```text
syrnck git catalog HEAD
syrnck git catalog release --directory PATH_TO_REPOSITORY --progress-json
syrnck git prepare-assets HEAD
```


## Transferts groupés {#grouped-transfers}

Le commit doit déjà exister localement. Store commun par défaut, options habituelles de clés/jetons/CA/compression/reprise/débit/tampon. Résultat : commit, assets, bundle et statistiques/session transfer.

Push prépare le bundle vérifié, transfère sa fermeture dédupliquée avec inventaire/bail persistants, puis publie la référence serveur git-COMMIT. Elle est immuable pour cette commande : une cible différente existante échoue avant transfert. Le CAS vérifie la fermeture entière. Même un changement de code seul possède sa référence de commit. Cette commande ne pousse pas Git ; répéter réutilise objets et reçus.

Pull calcule le bundle attendu depuis les pointeurs locaux et exige la référence serveur correspondante. Téléchargement dans git-stage/BUNDLE_ID, contrôle de tailles/hashes puis publication git-assets/COMMIT et retrait du staging. Échec/coupure peut laisser pin/journal ; refaire pull-assets après correction termine validation/publication. agent resume termine seulement le transfert de staging.

Aucun fichier de travail ou index Git ne change. Clone neuf : --no-checkout, git install, pull des assets voulus puis checkout explicite, ou politique download ci-dessous. Le [filtre résident](doc:git-filter-process) garde un Store par commande.

La transaction couvre un catalogue seulement. Plusieurs commits ont des racines séparées et les serveurs Git/blocs ne partagent pas de transaction distribuée. Un push Git échoué peut laisser des racines complètes : ne pas les retirer tant que l’histoire correspondante doit rester reconstructible.

```text
syrnck git push-assets https://blocks.example.invalid HEAD --requests 4 --require-lease
syrnck git pull-assets https://blocks.example.invalid HEAD --requests 4 --require-read-lease
```


## Checkout à la demande et clones de pointeurs {#on-demand-checkout-and-pointer-only-clones}

SYRNCK_TOKEN sert aux assets manquants ; SYRNCK_KEY ouvre le Store local chiffré initialisé avec --encrypted. Serveur et client peuvent avoir des clés différentes. HTTPS vérifie les certificats ; filtres générés avec CA système, invocation personnalisée pour --ca.

Politique par défaut local : objet absent échoue sans réseau de blocs. download choisit explicitement l’URL de blocs d’un remote Git. Manifeste/chunk absent déclenche un pull persistant du manifeste exact du pointeur stdin, avec bail de lecture serveur renouvelable obligatoire. Ni HEAD, fichier worktree ni catalogue ne déterminent la version : checkout sélectif/ancien fonctionne même avec HEAD ailleurs. Seuls objets absents de ce manifeste sont demandés. Le serveur doit conserver toute la fermeture via catalogues uploadés ; sans bail de lecture, download échoue.

Contenu ordinaire ou asset complet local : aucun jeton, endpoint ou réseau requis. Objet local corrompu : erreur, pas remplacement silencieux. Smudge vérifie taille du pointeur, tous les blocs et hash complet avant sortie, puis pin git/MANIFEST. Download échoué ne rend aucun octet reconstruit ; objets vérifiés/journal restent réutilisables. Le pin git-fetch/MANIFEST peut rester après taille invalide ; refaire checkout termine pin final et nettoyage. Resume générique termine seulement le staging.

skip garde les pointeurs canoniques sur disque, sans Store/clé/jeton/serveur. Clean accepte un pointeur non hydraté intact pour status/add, sans pin d’une fermeture incomplète. Pré-push/prepare exigent toujours tous les assets. Taille d’un manifeste présent incohérente ou corruption détectée fait échouer clean ; pointeurs mal formés échouent partout. SYRNCK_SKIP_SMUDGE=1 remplace la politique pour l’environnement/invocation, 0 garde la normale, autres valeurs refusées. Retirer la variable après usage si souhaité ; config locale non clonée, à installer explicitement par dépôt.

Changer de politique ne change aucun fichier. Git peut garder un pointeur dont index/stat correspondent même avec force. Pour hydrater un tel chemin, sauvegarder les modifications utiles, supprimer seulement son fichier pointeur puis git checkout -- CHEMIN. Une sélection dans un clone --no-checkout doit d’abord restaurer .gitattributes. Le code ordinaire garde Git.

La valeur locale syrnck.checkout vaut local, skip ou download:REMOTE. Les filtres installés passent --checkout-policy ; smudge manuel reste local sans cela. Seule l’ancienne commande générée exacte est mise à niveau ; une personnalisée est préservée par refus. Les filtres résidents traitent séquentiellement, sans hydratation différée ou checkout parallèle. Vérifier avant sortie impose une lecture complète, avec budgets d’objets/métadonnées et clés de fermeture, pas un tampon à la taille du fichier. Le contrat Git n’offre pas d’atomicité multi-fichier.

```text
git clone --no-checkout GIT_URL project
cd project
syrnck git install
syrnck git configure-remote origin https://blocks.example.invalid
syrnck git configure-checkout download --remote origin
git checkout --force HEAD
```


```text
syrnck git configure-checkout skip
git checkout --force HEAD
syrnck git configure-checkout local
```


## Hook pre-push explicite {#opt-in-pre-push-hook}

Installer le hook seulement avec install-hook ; git install seul ne le fait pas. remote.origin.syrnckUrl est distinct de l’URL Git ; configuration refuse identifiants intégrés, query/fragment et HTTP non loopback. Le script contient des chemins cités, hérite clés/jetons et transmet arguments/stdin ; aucun secret n’y est écrit. CA système par défaut ; intégrer manuellement --ca au hook existant si nécessaire.

Le hook résout chaque tête non supprimée en commit et parcourt toute l’ascendance locale, parents de fusion compris, sans croire les anciens commits distants. Tags annotés et pushes multi-références partagent ce parcours. Un asset retiré du dernier arbre doit encore être publié pour son ancien commit. Tout catalogue non vide est vérifié, uploadé et épinglé avant autorisation de Git ; baux d’envoi obligatoires.

Client blocs créé à la demande : suppressions seules ou histoire entièrement code sans endpoint/jeton/réseau. Un arbre actuel sans asset ne dispense pas de vérifier ses ancêtres. Manifeste absent, taille fausse, bloc indisponible, racine immuable incorrecte, bail non géré ou erreur réseau font échouer le hook et le push. Corriger puis relancer ; objets/reçus vérifiés se réutilisent.

Installation atomique/idempotente seulement pour le script généré exact. Hook différent, chemin lié ou core.hooksPath sont refusés sans écrasement. Un hook existant doit appeler l’équivalent syrnck --store COMMON_STORE git pre-push "$@", conserver stdin et propager l’échec. Worktrees partagent hooks/Store communs.

C’est un hook client contournable par --no-verify, sans enforcement GitHub/GitLab ou transaction distribuée. Annulation ou échec Git ultérieur peuvent laisser des racines conservées ; l’atomicité multi-ref appartient à Git. Contrôle serveur Git, expiration automatique des orphelins et publication distribuée restent à développer.

Histoires shallow/graft, tags non résolus en commits et données promisor absentes sont refusés : récupérer l’histoire complète auparavant. Bornes : 4096 lignes d’update, 1 Mio stdin, 4 Kio par ligne, 100000 commits uniques, sortie d’histoire 8 Mio. Les arbres sont rescannés à chaque appel ; index de présence/catalogue pour très grands projets reste à développer.

```text
syrnck git install
syrnck git configure-remote origin https://blocks.example.invalid
syrnck git install-hook
git push origin main
```


## Bornes et exigences Git {#bounds-and-local-git-requirements}

Git 2.45 minimum. --no-replace-objects et --no-lazy-fetch interdisent réécritures implicites/fetch de promisor absent. Énumération NUL avec tailles ; candidats pointeurs = blobs ordinaires de 1024 octets maximum. Les gros blobs de code ne sont pas chargés ; liens et sous-modules sont exclus, ces derniers demandant leur installation propre. Un blob commençant par l’en-tête exact Syrnck doit être canonique ; sinon c’est un blob ordinaire.

Arbre : 64 Mio et 262144 entrées. Blobs candidats uniques par lots de 128 via cat-file --batch, réponse 160 Kio. Stderr enfant 16 Kio. Le helper prépare les sorties en fichiers temporaires avant lecture bornée : cela ne borne ni disque temporaire ni tout le RSS. Annulation entre commandes/parsing/Store ; un enfant Git en cours peut terminer.
