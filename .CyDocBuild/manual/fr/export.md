---
id: export
title: Export de révisions
description: Guide : Export de révisions.
section: Stockage
order: 80
parent: storage
sourceRevision: 1
---

# Exporter une révision dans un nouveau dossier {#transactional-native-exports}

`native export REVISION NEW_DIRECTORY` vérifie la révision et sa fermeture complète, reconstruit chaque fichier dans un dossier frère de préparation puis publie l’arbre par renommage exclusif. La destination reste absente tant que l’ensemble n’est pas prêt. Aucun fichier, lien ou dossier existant n’est remplacé, même un dossier vide créé en concurrence.

`Store::export_revision(id, destination, OperationOptions)` accepte jeton d’arrêt et observateur synchrone. `verifying` couvre la fermeture initiale ; `exporting` compte les octets restaurés agrégés avec le total connu du snapshot et le chemin relatif courant, fichiers vides compris. Sa fin signifie préparation terminée. Le `publishing` suivant est le dernier callback annulable avant renommage. Le succès de la commande ou du retour confirme la publication. Références et objets source restent intacts.

La préparation `.syrnck-export-<32 chiffres hexadécimaux minuscules>` est créée dans le parent existant de la destination pour rester sur le même système de fichiers. Sous Unix, le dossier est privé au propriétaire et garde ces permissions après publication. Les fichiers préparés sont décodés ; le bit exécutable est rétabli sous Unix. Annulation, exception d’observateur ou erreur ordinaire nettoient seulement la préparation de cet appel. Les chemins proviennent des révisions validées et les manifestes sont complets.

Le renommage utilise MoveFileExW sans remplacement sous Windows, `renameat2(RENAME_NOREPLACE)` sous Linux et `renamex_np(RENAME_EXCL)` sous macOS. Si la primitive exclusive n’est pas disponible, l’export échoue et retire sa préparation ; aucun repli susceptible d’écraser une destination n’est utilisé. Les fichiers sont synchronisés individuellement ; sous Unix, les entrées de dossiers le sont avant publication et le parent après. Une annulation après engagement du renommage peut arriver trop tard. Une erreur de durabilité finale peut laisser un arbre complet publié ; aucune garantie universelle de panne d’alimentation n’en découle.

Un arrêt forcé peut laisser une préparation, avec destination absente ou complète. Le lecteur ne la nettoie pas automatiquement : vérifier son appartenance et l’utilité des données avant suppression. Réessayer vers une destination absente prépare un nouvel arbre. Ces fichiers temporaires ne sont pas des objets de révision ni des racines de rétention.

La reconstruction lit les blocs en flux, mais la fermeture publique matérialise encore son ensemble de clés et les limites JSON des révisions/manifestes restent valables. Pour modifier un workspace existant, utiliser le [checkout récupérable](doc:native-checkout), distinct de cet export vers un nouveau dossier.

```text
syrnck native export REVISION restored-project --progress
syrnck native export REVISION restored-project --progress-json
```
