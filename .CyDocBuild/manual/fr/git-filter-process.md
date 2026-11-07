---
id: git-filter-process
title: Filtres Git résidents
description: Guide : Filtres Git résidents.
section: Git
order: 120
parent: git-assets
sourceRevision: 1
---

# Filtres Git résidents {#resident-git-filters}

git install configure aussi filter.syrnck.process localement. Une commande Git envoie plusieurs demandes clean/smudge au même processus ; le CLI ouvre son Store à la demande et conserve son cache vérifié borné jusqu’à fermeture de stdin, sans service de fond.

Les anciens filtres générés restent disponibles et l’ancienne commande smudge exacte peut être mise à niveau. Toute autre commande clean/smudge/process existante est refusée avant écriture. Sans process hérité, retirer localement filter.syrnck.process réactive les filtres simples ; git install remet le résident. Les filtres personnalisés nécessitent une intégration manuelle.

```text
syrnck git install
git add Content
git checkout another-branch
```


## Comportement et portée des gains {#behavior-and-performance-scope}

Même moteur clean/smudge et mêmes politiques local/download/skip. Configuration/environnement sont lus au lancement. Skip ne demande ni Store ni clé, même toute une session ; clean demande toujours le stockage local. Download crée le client distant seulement pour un objet manquant et exige les baux de lecture renouvelables. Les objets locaux vérifiés restent hors ligne ; journaux et pins conservent leurs contrats.

Le Store garde son verrou exclusif pendant toute la commande Git, attentes entre demandes comprises ; les autres commandes du même Store échouent rapidement si occupé. Requêtes séquentielles. Les worktrees partagent le Store commun sur disque, pas le cache des processus indépendants. Aucune capacité delay, planification parallèle, daemon ou transaction de checkout multi-fichier n’est introduite.

Le handle évite des initialisations répétées et réutilise les décodages vérifiés. Chaque accès relit et empreinte néanmoins l’enveloppe courante pour détecter les corruptions. La vérification complète avant sortie smudge reste obligatoire ; aucun débit Unreal n’est garanti.

## Flux et bornes {#streaming-and-bounds}

Le protocole filter-process négocie la version 2 et seulement clean/smudge proposés. Les corps binaires utilisent pkt-line avec 65516 octets de payload maximum par paquet, données vides distinctes du flush, sans transformation. Les lignes texte acceptent un LF final facultatif. Métadonnées : 64 Kio et 256 enregistrements ; en-têtes inconnus ignorés, clés dupliquées ou commande/pathname absent/non géré font échouer le fichier. Le pathname est un contexte emprunté, jamais un chemin ouvert par l’adaptateur.

Aucune réponse avant le flush de fin du corps. Clean ingère directement en flux puis émet le petit pointeur. Smudge consomme le pointeur, vérifie sa fermeture et reconstruit directement dans les paquets, sans copie temporaire complète d’asset pour le framing.

Un handler générique peut produire avant d’avoir fini de lire, comme le passthrough brut de smudge. Au plus 64 Kio sont gardés avant déversement dans un flux temporaire privé. Celui-ci n’est relu par paquets qu’après fin de l’entrée et se ferme/supprime avec la demande, même en échec. Il contient du clair et peut croître avec tout le fichier brut ; le chiffrement du Store ne le protège pas. Paquets, caches, pipeline, clés visitées et métadonnées s’ajoutent : aucun plafond global RSS/disque temporaire.

## Erreurs et fin de processus {#failures-and-shutdown}

Une erreur de fichier draine le reste de son entrée, écrit le diagnostic traduit sur stderr et répond status=error. La demande suivante peut réussir. Si la sortie avait commencé, la liste finale de statut annule le succès précédent : Git doit rejeter le fichier. filter.syrnck.required=true impose cette règle. Aucun diagnostic ne passe sur stdout binaire.

Framing invalide, métadonnées excessives, erreur IO protocole ou annulation terminent le processus. Codes : 130 pour arrêt coopératif, 1 pour erreur fatale, 0 pour EOF normal entre demandes, même après erreur de fichier. Le client doit donc lire les statuts encadrés, pas seulement le code du processus. Une lecture de pipe bloquée attend données/EOF avant de terminer l’annulation.

L’API git::filter_process reçoit des handlers non encadrés, sans HTTP, avec callback facultatif de diagnostic pour erreurs récupérables et exceptions pour erreurs fatales. Respecter l’annulation, les contrôles de sortie et la sérialisation Store/observateur.
