---
id: overview
title: Syrnck
description: Syrnck
section: Syrnck
order: 0
sourceRevision: 3
---

## Choisir son parcours {#getting-started}

Syrnck est un moteur C++20 de stockage de blocs et de révisions pour les assets volumineux. Chaque version possède un manifeste complet : sa reconstruction n’exige pas de rejouer des patches binaires. Les blocs communs se réutilisent entre versions, chemins et transferts.

Commencer par le [parcours de démarrage](doc:usage), puis choisir les [assets Git](doc:git-assets) ou l’[historique natif](doc:native-history). Inspecter le [workspace](doc:native-workspace) avant les changements et conserver une [sauvegarde vérifiée](doc:backups) avant tout retrait d’anciennes versions. Le [guide du stockage](doc:storage) explique leur rétention.

## Fonctions disponibles {#workflows}

Git conserve l’historique ordinaire du code, les branches, revues, fusions et autres fonctions Git ; les filtres Syrnck représentent les assets choisis par des pointeurs. Héberger ces pointeurs sur GitHub/GitLab ne fournit pas un serveur de blocs Syrnck : les données partagées demandent un stockage/serveur séparé.

Le moteur autonome propose commits, index figé, branches, tags, checkout récupérable, merge, rebase linéaire/interactif, cherry-pick, revert, squash, reset, stash, historique des renommages exacts et bisect. Chaque guide précise les règles de conflit et de publication. Les [transferts](doc:transfers) ne publient que des fermetures complètes vérifiées. L’[agent résident de publication](doc:resident-agent) surveille les changements locaux et reprend les publications natives, y compris après des modifications hors ligne. Son mode bidirectionnel facultatif reçoit les révisions et fusionne les historiques concurrents compatibles. Les [permissions et baux d’assets](doc:server-access) contrôlent les serveurs configurés.

Les [intégrations C++/C/Go](doc:integrations) incluent un [adaptateur BEP Syncthing](doc:syncthing) pour fichiers ordinaires, sans constituer un daemon Syncthing résident. Aucun plugin Unreal n’est fourni ; le moteur stocke des octets d’assets sans interpréter leurs objets Unreal.

## Lire ce manuel local {#reading}

Le lecteur complet fonctionne par file:// sans serveur ni connexion Internet. Rechercher texte et code dans l’en-tête ou avec Ctrl+K. L’arbre de navigation accepte Tab/Entrée et les flèches ; un bouton de menu apparaît sur les petits écrans. Le thème suit initialement le système puis se choisit dans son menu. Anglais et français partagent les IDs de pages et sections. Une langue indisponible revient à l’anglais en gardant la destination.

Signets, notes et préférences dépendent du stockage local autorisé par le navigateur. Les positions sont gardées pendant la navigation ; les liens de section explicites sont prioritaires. Les références externes sont facultatives et ne demandent Internet que lorsqu’on les ouvre.

Utiliser `syrnck help`, `syrnck help native-tags` ou `syrnck --lang fr help native-tags`. `--section ID` ouvre une section précise ; `--print-url` affiche l’adresse sans lancer de navigateur ; `--directory PATH` choisit un autre dossier Documentation installé. `--help` reste la référence courte du terminal. Cette CLI ne possède ni menu d’aide ni interface F1 à remplacer.

## Limites actuelles {#limits}

Lire l’[architecture](doc:architecture), les [formats](doc:protocol) et les [limites](doc:limits) avant intégration ou déploiement. Le chiffrement protège les objets au repos, tandis que les transferts fournissent des objets décodés au serveur de confiance. Chiffrement de bout en bout, rétention distribuée et service complet de synchronisation continue restent à développer. Le code original de Syrnck est sous MIT ; les [notices des dépendances](doc:licenses) s’appliquent séparément.
