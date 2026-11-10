---
id: key-files
title: Clés de stockage protégées par mot de passe
description: Créer, vérifier et reprotéger les fichiers de clés ; ouvrir les Stores natifs sans exporter les clés.
section: Storage
parent: storage
order: 55
sourceRevision: 5
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

## Coffres de clés système {#system-vault}

L'[API C++ native des coffres](doc:api-system-key-vault) conserve des clés de stockage, graines de signature et clés maîtresses scellées indépendantes dans le compte système courant. Elle est distincte des fournisseurs de fichiers protégés ci-dessus. La sélection par environnement CLI et les propriétaires C/Go ne choisissent pas encore ce fournisseur.

```cpp
#include <syrnck/key_vault.hpp>

syrnck::SystemKeyVault vault;
auto reference = vault.generate(syrnck::VaultKeyPurpose::sealed);
auto master = vault.open_secret(reference);
// Conserver reference.encode() dans la configuration locale publique.
// Exporter une sauvegarde protégée avant de dépendre du coffre de ce compte.
auto backup = vault.export_protected(reference, backup_password);
```

`generate`, `save` et `import_protected` créent de nouvelles entrées avec des identifiants aléatoires de 256 bits. Aucune énumération, aucun nom de cible choisi par l'appelant, aucune mise à jour. Les références portent une version, un usage et un identifiant hexadécimal minuscule ; l'enregistrement associe ce même usage/identifiant au secret. `open_signer` rend un propriétaire natif de signature sans export de graine brute. `export_protected` utilise le format existant de 104 octets, authentifié et distinct par usage. L'appelant possède ses buffers de mots de passe et doit enregistrer la sauvegarde hors du workspace. Une référence seule ne restaure pas un compte ou une machine perdus.

| Plateforme | Fournisseur et prérequis |
| --- | --- |
| Windows | Identifiants génériques du Gestionnaire d'identification, compte courant sur cet hôte (`CRED_PERSIST_LOCAL_MACHINE`). Une session utilisable est nécessaire ; une session réseau seule ou cloisonnée peut refuser l'accès. Aucune itinérance d'entreprise demandée. |
| macOS | Mots de passe génériques du Trousseau. La création utilise le trousseau par défaut ; un chemin absolu optionnel au constructeur choisit un trousseau existant sans modifier le défaut ni la liste de recherche. Les opérations demandent un échec plutôt qu'une fenêtre d'authentification. Le propriétaire du compte doit configurer déverrouillage et confiance. |
| Linux | libsecret et le bus de session Secret Service du compte. L'écriture exige une collection par défaut existante et déverrouillée. Les lectures ne demandent pas de déverrouillage ; plusieurs enregistrements correspondants sont refusés. Les appels partagent une échéance d'annulation de 30 secondes par opération du backend. Les règles du fournisseur peuvent exiger une invite à la création/suppression ; un hôte résident doit disposer d'une session autorisée déverrouillée. |

Configurer `SYRNCK_SYSTEM_KEY_VAULT=AUTO` (défaut), `ON` (obligatoire) ou `OFF`. Sous Linux, les fichiers de développement `libsecret-1` présents activent le fournisseur ; un SDK installé construit ainsi exige aussi les fichiers de développement libsecret. `SystemKeyVault::provider()` indique `windows-credential-manager`, `macos-keychain`, `linux-secret-service` ou `unavailable` ; cela ne prouve pas que la session du compte est déverrouillée. Un fournisseur/une session absent, un coffre verrouillé, une référence absente ou un enregistrement invalide échoue explicitement, sans repli en clair ou vers un fichier. Le stockage du fournisseur, les copies système/bibliothèque et les permissions du compte relèvent de l'OS. Cela n'isole pas les clés dans du matériel et ne garantit pas qu'un autre processus du même compte ne lise pas les identifiants. L'API d'écriture Windows remplace une cible exacte existante ; Syrnck génère un nouvel identifiant imprévisible et vérifie son absence, sans proposer de contrat atomique de création exclusive à un nom choisi par l'appelant.

Supprimer une entrée n'invalide pas les propriétaires natifs existants, ne renouvelle pas le chiffrement du dépôt, ne révoque pas les copies exportées et ne retire pas une autorisation serveur. Exporter/vérifier des sauvegardes protégées indépendantes avant un changement de compte. Les sauvegardes ordinaires du Store ne capturent pas le coffre OS. La qualification réelle optionnelle utilise `SYRNCK_TEST_SYSTEM_VAULT=ON` et `key_vault_system` ; Linux lance un bus/trousseau séparé, macOS crée et supprime un trousseau de test explicitement choisi, Windows crée/retire uniquement ses entrées synthétiques aléatoires.

## Travaux de sécurité restants {#limits}

Les fournisseurs de fichiers protégés ouvrent le chiffrement au repos existant. Le [workflow chiffré explicite](doc:sealed-repositories) utilise une clé maîtresse indépendante et une finalité de protection distincte pour le transport opaque. Le coffre système C++ séparé n'intègre pas encore la sélection des fournisseurs CLI/C/Go ni la rotation/révocation distribuée. La signature utilise des [signataires protégés distincts](doc:signing-keys). Checkout et staging de téléchargement persistant peuvent contenir du clair. Voir les [limites de sécurité actuelles](doc:limits#security).
