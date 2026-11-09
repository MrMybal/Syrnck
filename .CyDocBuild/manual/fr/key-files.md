---
id: key-files
title: Clés de stockage protégées par mot de passe
description: Créer, vérifier et reprotéger les fichiers de clés ; ouvrir les Stores natifs sans exporter les clés.
section: Storage
parent: storage
order: 55
sourceRevision: 4
---

## Créer un fichier de clé {#create}

Définir `SYRNCK_KEY_PASSWORD` depuis un lanceur de confiance, puis créer le fichier dans un dossier privé existant hors du projet. Les mots de passe ne sont jamais des arguments de commande ni affichés en sortie. Le texte de l'environnement Windows est converti en UTF-8, sans normalisation ni retrait des espaces. La limite native est de 1 à 4096 octets.

```text
syrnck key create private-keys/project.syrkey
syrnck key verify private-keys/project.syrkey
syrnck key inspect private-keys/project.syrkey
```

`create` génère une clé de stockage aléatoire de 32 octets et écrit uniquement son enveloppe authentifiée de 104 octets. `inspect` lit les métadonnées publiques de dérivation/chiffrement et indique `authenticated: false`, sans vérifier mot de passe ni texte chiffré. `verify` authentifie et indique `authenticated: true`, sans exporter la clé.

Les coûts Argon2id par défaut sont de 64 Mio et trois passes. `create`, `protect` et `rewrap` acceptent `--memory-mib 64..256` et `--passes 3..10`. `--password-env NOM` choisit une autre variable au nom ASCII majuscule. Choisir les droits d'accès du dossier parent privé ; Unix crée les fichiers en mode 0600, tandis que Windows utilise la sécurité héritée/par défaut du système de fichiers pour ce dossier.

## Protéger une clé brute existante {#existing}

Pour un Store déjà chiffré, conserver sa clé `SYRNCK_KEY` exacte de 64 caractères hexadécimaux, définir `SYRNCK_KEY_PASSWORD`, puis exécuter :

```text
syrnck key protect private-keys/project.syrkey
```

La même clé est protégée ; aucun objet ni référence du Store ne change. `key create` génère toujours une nouvelle clé : utiliser `protect` pour garder l'accès à un Store existant.

## Ouvrir la CLI et l'agent résident {#providers}

Définir `SYRNCK_KEY_FILE` vers le chemin absolu du fichier d'enveloppe et `SYRNCK_KEY_PASSWORD` vers son mot de passe. Retirer `SYRNCK_KEY` : configurer les deux fournisseurs est refusé. Initialiser explicitement un nouveau Store chiffré :

```text
syrnck --store project/.syrnck init --encrypted
syrnck --store project/.syrnck fsck
```

Le fournisseur de fichier s'applique aux commandes natives, Git, serveur et agent résident qui ouvrent un Store. Identifiants absents/incorrects, fichiers mal formés et entrées liées sont refusés sans repli en clair. L'entrée doit être un fichier ordinaire non lié contenant exactement 104 octets. Les processus résidents réutilisent les clés authentifiées pour des octets d'enveloppe/mot de passe identiques ; tout changement impose une nouvelle authentification. Redémarrer le processus pour changer son environnement hérité.

L'exécutable facultatif autonome `syrnck-bep` accepte `key_file` et `key_password_env` dans sa [configuration locale](doc:syncthing#standalone-service). Les chemins relatifs partent du fichier de configuration. Il authentifie une seule fois au démarrage et conserve un propriétaire natif jusqu'à l'arrêt des workers. Les changements d'enveloppe/mot de passe prennent effet après redémarrage. Les applications utilisant la bibliothèque de service Go peuvent fournir ce même propriétaire dans leur callback `OpenStore`.

## Changer de mot de passe {#password-change}

Définir l'ancien `SYRNCK_KEY_PASSWORD` et le nouveau `SYRNCK_NEW_KEY_PASSWORD` :

```text
syrnck key rewrap private-keys/project.syrkey private-keys/project-new.syrkey
```

La destination doit être nouvelle. L'original reste intact, les coûts non précisés sont conservés, et un nouveau sel/nonce protège la même clé de stockage. `--new-password-env NOM` choisit une autre variable pour le nouveau mot de passe. Modifier chemin et mot de passe du fournisseur après vérification du nouveau fichier. Les destinations existantes, liens inclus, ne sont jamais remplacées. Une erreur de publication peut suivre l'apparition du fichier ; inspecter/vérifier la destination avant de réessayer.

Les anciennes enveloppes restent utilisables avec leurs anciens mots de passe. Cette opération ne révoque pas les clés, ne renouvelle pas le chiffrement des objets du Store et ne récupère pas un mot de passe oublié. Garder une autre copie protégée selon votre politique de sauvegarde ; les [sauvegardes du Store](doc:backups) ne conservent pas automatiquement les fichiers de clés externes.

## Intégration C, C++ et Go {#sdk}

L'[API C++](doc:api-password-keys) détient les octets secrets dans `SecretKey`, déplaçable sans copie. L'[ABI C](doc:api-c-abi#protected-keys) et le [binding Go](doc:go-binding#protected-keys) fournissent des propriétaires natifs opaques : authentifier une fois, ouvrir des sessions Store courtes, puis fermer explicitement le propriétaire. Sa fermeture n'invalide pas les Stores ouverts, qui gardent leurs propres clés. Les buffers de mots de passe appartiennent toujours à l'appelant.

## Travaux de sécurité restants {#limits}

Ces fournisseurs ouvrent le chiffrement au repos existant. Le [workflow chiffré explicite](doc:sealed-repositories) utilise une clé maîtresse indépendante et une finalité de protection distincte pour le transport opaque. Aucun de ces fournisseurs ne fournit de coffre OS ou de rotation/révocation distribuée. La signature utilise des [signataires protégés distincts](doc:signing-keys). Checkout et staging de téléchargement persistant peuvent contenir du clair. Voir les [limites de sécurité actuelles](doc:limits#security).
