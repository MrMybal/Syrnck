---
id: limits
title: Limites actuelles et travail prévu
description: Limites actuelles et travail prévu
section: Syrnck
order: 900
sourceRevision: 2
---

## Workspace et versionnage {#workspace}

[HEAD nommé et détaché](doc:native-detached) sont pris en charge. Les workspaces natifs multiples/partiels restent à compléter. L’index de travail persistant et l’observateur évitent certains recalculs de hash, sans produire un snapshot atomique du système de fichiers ni un historique complet d’événements. Les notifications OS peuvent déborder ; réanalyses prudentes et polling restent nécessaires.

Les renommages de contenu exact sont reconnus, mais renommages avec édition/copies, intégration aux merges/rebases et synthèse récursive des bases multiples restent incomplets. Les révisions natives v1 n’ont ni auteur ni signature. Respecter les limites de chaque mode de fusion/rebase : la présence d’une commande ne promet pas toute la compatibilité Git.

## Équipe et synchronisation continue {#sync}

Les serveurs configurés fournissent utilisateurs/rôles, contrôles de branches et baux d’assets expirables. Transfert de l’historique natif et publication des catalogues Git restent distincts d’une transaction distribuée multi-serveur. L’agent de transfert est ponctuel. La surveillance locale ne constitue pas un service résident complet avec propagation des suppressions/renommages, conflits hors ligne et réconciliation causale.

L’adaptateur Syncthing traite index/requêtes/réponses de blocs de fichiers ordinaires. Base de données, découverte, association d’appareils, vecteurs de versions, copies conflictuelles et synchronisation complète de dossiers relèvent de l’application hôte ou de travaux futurs.

## Sécurité, rétention et récupération {#security}

Les objets peuvent être chiffrés au repos, avec clés indépendantes pour sauvegardes/restaurations. Les serveurs de confiance reçoivent les objets en clair dans le transport sécurisé. Chiffrement de bout en bout, dérivation de mot de passe, coffres système, rotation/révocation distribuée, réparation automatique de répliques et GC distribué restent à développer.

La rétention est explicite. Les archives conservent les versions tant que leurs racines restent présentes ; une sauvegarde vérifiée peut protéger le retrait manuel, sans politique automatique par âge/release. Arrêt de processus et panne d’alimentation sont des garanties différentes. Aucune opération n’établit une sûreté universelle après coupure électrique, sur stockage réseau ou face à un écrivain externe hostile.
