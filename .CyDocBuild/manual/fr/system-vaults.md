---
id: system-vaults
title: Utiliser les coffres système
description: Créer des clés natives, exporter des sauvegardes protégées et configurer le CLI ou les services résidents.
section: Storage
parent: key-files
order: 56
sourceRevision: 1
---

## Créer et inspecter des clés {#commands}

Les commandes `vault` utilisent le [fournisseur système](doc:key-files#system-vault) du compte OS courant. `provider` indique le support compilé sans ouvrir le coffre. Un fournisseur disponible exige encore une session de compte autorisée et déverrouillée. Aucun échec ne sélectionne un autre fournisseur de clé.

```text
syrnck vault provider
syrnck vault create storage private-keys/project.vault.json
syrnck vault create signing private-keys/author.vault.json
syrnck vault create sealed private-keys/team.vault.json
syrnck vault inspect private-keys/author.vault.json
```

Utiliser un dossier parent existant. La création génère une clé indépendante et une référence aléatoire ; aucun fichier existant n’est remplacé. `inspect` authentifie l’entrée exacte sans retourner son secret. La commande retourne la clé publique/l’identifiant de signature ou le domaine scellé selon l’usage.

Sous macOS, `create` et `import` acceptent `--keychain CHEMIN_ABSOLU` vers un trousseau fichier existant. Le chemin choisi est inscrit dans le fichier de référence. Le CLI sans interface et l’exécutable BEP autonome désactivent explicitement toute interaction Trousseau de leurs propres processus lorsqu’un coffre est sélectionné. Ils ne changent pas les valeurs par défaut du compte ni les règles d’une autre application. Déverrouiller le trousseau et autoriser l’exécutable consommateur restent la responsabilité du propriétaire du compte. La confiance accordée au CLI n’autorise pas automatiquement un autre exécutable comme `syrnck-bep`.

## Fichiers de référence publics {#references}

Un fichier de référence contient des sélecteurs locaux publics, pas une clé ou une sauvegarde portable :

```json
{
  "version": 1,
  "type": "system-vault-reference",
  "provider": "windows-credential-manager",
  "reference": "syrnck-vault:1:storage:0000000000000000000000000000000000000000000000000000000000000000"
}
```

L’identifiant nul ci-dessus est un exemple. Utiliser une référence générée. Le schéma exact n’autorise que ces quatre champs et la chaîne optionnelle `keychain` pour macOS. Membres dupliqués/inconnus, versions non prises en charge, références invalides, liens/fichiers non ordinaires et fichiers dépassant 16 Kio sont refusés. Inspection/export/suppression vérifient le fournisseur enregistré. Ces actions n’acceptent pas de remplacement du chemin de trousseau. Le JSON de sortie standard comporte aussi des champs de résultat ; utiliser le fichier écrit par la commande comme reçu.

## Sélectionner les fournisseurs CLI {#providers}

Affecter à chaque variable la **chaîne `reference`** du reçu, pas son nom de fichier :

| Usage | Sélecteur de coffre | Autres fournisseurs, mutuellement exclusifs |
| --- | --- | --- |
| Chiffrement du Store | `SYRNCK_KEY_VAULT` | `SYRNCK_KEY`, `SYRNCK_KEY_FILE` |
| Signature des révisions natives | `SYRNCK_SIGNING_KEY_VAULT` | `SYRNCK_SIGNING_SEED`, `SYRNCK_SIGNING_KEY_FILE` |
| Maître du transport scellé | `SYRNCK_SEALED_KEY_VAULT` | `SYRNCK_SEALED_KEY`, `SYRNCK_SEALED_KEY_FILE` |

`SYRNCK_VAULT_KEYCHAIN` choisit éventuellement un chemin absolu de trousseau fichier macOS pour ces fournisseurs d’environnement. Les autres plateformes refusent cette option. Sélecteur vide/invalide, usage de clé différent ou sélection simultanée de clé brute/fichier/coffre échouent. Utiliser `init --encrypted` avec une clé de stockage. Une clé de signature exige toujours une [identité d’auteur native](doc:native-authorship) ; le maître scellé reste indépendant de la clé de stockage au repos.

Les agents CLI résidents gardent en cache les propriétaires natifs de stockage/signature authentifiés selon la référence exacte et le trousseau choisi ; les courtes sessions Store ne relisent donc pas le coffre OS. Les variables de signature/clé scellée sont retirées de l’environnement propre du CLI après consommation, ou avant le lancement d’enfants par des commandes sans rapport. L’environnement du lanceur reste inchangé. Redémarrer pour abandonner les clés conservées ou changer les règles du compte/de confiance ; supprimer l’entrée ne révoque pas les propriétaires existants.

## Configurer BEP autonome {#services}

Utiliser les références publiques encodées dans le JSON habituel du service. Cet extrait exige les autres [paramètres du service](doc:syncthing#standalone-service) :

```json
{
  "key_vault": "syrnck-vault:1:storage:0000000000000000000000000000000000000000000000000000000000000000",
  "author": {"name": "Project author", "email": "author@example.invalid", "timezone_offset": 0},
  "signing_key_vault": "syrnck-vault:1:signing:1111111111111111111111111111111111111111111111111111111111111111"
}
```

Remplacer les deux références d’exemple. `key_vault` exclut `key_file` et une valeur brute non vide dans `key_env` (`SYRNCK_KEY` par défaut). `signing_key_vault` exige `author` et exclut `signing_key_env`/`signing_key_file`. Les paramètres de variable de mot de passe concernent uniquement les fichiers protégés. `vault_keychain` optionnel doit être un chemin macOS absolu et exige au moins un sélecteur de coffre. Les rôles/configurations invalides échouent avant le démarrage des workers/listeners. L’authentification charge des propriétaires natifs typés avant capture/activité réseau et les conserve jusqu’à l’arrêt ; aucune clé/graine brute n’entre dans Go. Les nouvelles révisions exigent une signature lorsqu’un signataire est configuré. Redémarrer pour changer les identifiants.

Les sélecteurs explicitement vides/null et les alias de casse des nouveaux membres de coffre sont refusés. Le rôle récepteur chiffré opaque peut utiliser une clé de stockage du coffre mais refuse toujours les options d’auteur/signature. Les clés privées d’appareil TLS restent des fichiers PEM. Les mots de passe de dossier Syncthing et les maîtres scellés natifs sont des protocoles/identifiants distincts.

## Sauvegarder, restaurer et supprimer {#recovery}

Définir `SYRNCK_KEY_PASSWORD` depuis un lanceur de confiance, ou choisir un autre nom ASCII majuscule avec `--password-env NOM` :

```text
syrnck vault export private-keys/project.vault.json private-keys/project.syrkey
syrnck vault import storage private-keys/project.syrkey private-keys/recovered.vault.json
syrnck vault inspect private-keys/recovered.vault.json
syrnck vault erase private-keys/project.vault.json
```

`export` écrit l’enveloppe de mot de passe existante de 104 octets, distincte par usage ; `--memory-mib 64..256` et `--passes 3..10` configurent Argon2id. `import` l’authentifie avant de créer une nouvelle référence sur le compte/fournisseur de destination. Les enveloppes de stockage/signature/clé scellée ne sont pas interchangeables. Les sorties doivent être de nouveaux fichiers. Vérifier des sauvegardes indépendantes avant de dépendre du coffre d’un compte ; les sauvegardes du Store ne l’incluent pas.

`erase` retire seulement l’entrée exacte choisie, retourne `removed: false` si elle est absente et conserve le reçu public. Cette action ne révoque ni clés actives/en cache, ni copies exportées, ni autorisations serveur ; elle ne renouvelle pas le chiffrement du dépôt et ne retire pas l’historique scellé.

Écriture OS et publication du reçu sur le système de fichiers sont séparées. Une erreur de publication peut survenir après apparition du fichier. La commande conserve l’entrée OS et indique sa référence publique encodée. Établir le résultat avant de recommencer ; reconstruire le reçu public strict avec la référence signalée, le fournisseur compilé et le trousseau initialement choisi pour inspecter/exporter/supprimer explicitement l’entrée. Un échec d’écriture OS peut aussi laisser un résultat fournisseur incertain. Ces opérations ne forment pas une transaction entre systèmes.

## Qualification {#qualification}

Les tests par défaut `key_vault_cli` valident commandes, reçus stricts, exclusion des fournisseurs et refus avant création du Store sans accéder au compte. `SYRNCK_TEST_SYSTEM_VAULT=ON` explicite ajoute les vérifications CLI lorsque le CLI est construit. `SYRNCK_TEST_SYSTEM_VAULT_GO=ON` inclut les propriétaires Go natifs ; `SYRNCK_TEST_SYSTEM_VAULT_BEP=ON` ajoute le vrai démarrage/arrêt BEP et la capture signée hors ligne. Les deux sondes Go vérifient strictement les pointeurs. Linux utilise un bus/trousseau isolé ; macOS un trousseau temporaire choisi ; Windows seulement des entrées aléatoires appartenant au test. Fournisseurs absents/verrouillés échouent sans repli en clair. Les utilisateurs de bibliothèque choisissent toujours eux-mêmes la règle explicite du processus macOS.
