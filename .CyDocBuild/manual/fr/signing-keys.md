---
id: signing-keys
title: Clés de signature de révision protégées
description: Protéger les graines Ed25519 et conserver un propriétaire natif authentifié pour les services.
section: Sécurité
parent: native-authorship
order: 80
sourceRevision: 2
---

## Créer un signataire protégé {#create}

Définir `SYRNCK_SIGNING_KEY_PASSWORD` via un lanceur de confiance et utiliser un
dossier privé existant hors de l’arbre de travail. Le mot de passe n’apparaît ni
dans les arguments ni dans la sortie. Sous Windows, conversion UTF-8 exacte ;
1..4096 octets sans normalisation ni suppression d’espaces.

```text
syrnck signing-key create private-keys/author.syrsig
syrnck signing-key inspect private-keys/author.syrsig
syrnck signing-key verify private-keys/author.syrsig
```

`create` génère une graine Ed25519 et écrit uniquement son enveloppe chiffrée de
104 octets. La sortie inclut public_key/key_id authentifiés. `inspect` rend les
métadonnées publiques KDF/chiffrement avec `authenticated: false` sans annoncer
une identité publique non authentifiée. `verify` authentifie et rend la clé
publique, jamais la graine. Coûts Argon2id : `--memory-mib 64..256` et
`--passes 3..10` ; défaut 64 Mio et trois passes. `--password-env NOM` choisit une
autre variable en majuscules. Création exclusive ; mode Unix 0600 et permissions
héritées du dossier privé sous Windows.

## Protéger une graine existante {#existing}

Pour un signataire existant, définir sa graine exacte de 64 caractères hexadécimaux
`SYRNCK_SIGNING_SEED` et le mot de passe, puis exécuter
`syrnck signing-key protect private-keys/author.syrsig`. L’identité publique reste
identique. Les clés de stockage ont une autre destination : les commandes `key`
et fournisseurs de stockage refusent les enveloppes de signature, et inversement.

## Changer le mot de passe {#password-change}

Définir l’ancien mot de passe et `SYRNCK_NEW_SIGNING_KEY_PASSWORD`, puis créer un nouveau fichier :

```text
syrnck signing-key rewrap private-keys/author.syrsig private-keys/author-new.syrsig
```

`--new-password-env NOM` choisit une autre variable de nouveau mot de passe.
Sel/nonce sont renouvelés ; signataire et coûts précédents sont conservés sauf
option explicite. L’ancienne enveloppe reste utilisable : aucun retrait de clé
ni changement de politique serveur. Sauvegarder enveloppe et identifiants ;
un mot de passe oublié n’est pas récupérable.

## Configurer le CLI {#configure}

Définir `SYRNCK_SIGNING_KEY_FILE` et son mot de passe. Laisser
`SYRNCK_SIGNING_SEED` absente. Configurer un auteur natif puis capturer :

```text
syrnck native identity --name "Project author" --email author@example.invalid
syrnck native commit -m "Signed change"
```

Le fichier et la graine brute sont mutuellement exclusifs, même si la variable
brute existe mais est vide. Identifiants absents/vides, fichiers non ordinaires/
liés, en-têtes invalides, mauvaise destination ou échec d’authentification bloquent
les opérations signées sans repli non signé. Une configuration invalide préserve
l’historique. Fournir un signataire impose les signatures des nouvelles révisions
pour cette identité dans le processus. Le profil conserve seulement nom/email/
fuseau publics, sans chemin du fichier, mot de passe, graine ou politique obligatoire.
Garder le fournisseur dans le lanceur de confiance ; les
[règles serveur](doc:server-signatures) imposent les signatures à toute l’équipe.

Le CLI efface les variables de signature avant qu’une commande sans rapport
puisse lancer des enfants. L’agent natif authentifie une seule fois au démarrage
et conserve son signataire natif déplaçable entre sessions courtes du Store.
Suppression/changement du fichier ou mot de passe prend effet au redémarrage ;
un fournisseur configuré indisponible empêche ce démarrage. Les cibles de checkout
signées déjà préparées restent récupérables sans refaire leur signature.

## Propriétaires natifs et services {#services}

L’API C++ utilise le même format avec destination authentifiée :

```cpp
#include <syrnck/signing_keys.hpp>
auto sealed = syrnck::generate_protected_signing_key(password);
auto signer = syrnck::open_protected_signing_key(sealed.envelope, password);
store.use_native_identity(identity, std::move(signer), true);
```

Les fonctions additives ABI-1 `signing_key_protect/change_password/open/close`,
`signing_key_public_json` et `store_use_native_signer` fournissent un propriétaire
opaque. Les requêtes de taille n’authentifient ni ne génèrent de clé. Seul le JSON
public est exporté. Sérialiser les appels et fermer chaque handle C une fois.

Go fournit `ProtectSigningKey`, `ChangeSigningKeyPassword` et
`OpenProtectedSigner`. Ne pas copier `SigningKey` ; Close est explicite/idempotent.
`NativeIdentityOptions.Signer` exclut Seed. Chaque Store obtient son propre état
de signature développé ; fermer le propriétaire n’invalide pas les Stores déjà
configurés. Aucune graine brute n’est exportée vers Go. Utiliser des versions
compatibles des en-têtes, binding et bibliothèque native.

```go
owner, err := syrnck.OpenProtectedSigner(envelope, password)
if err != nil { return err }
defer owner.Close()
return store.ConfigureNativeIdentity(identity, syrnck.NativeIdentityOptions{
    Signer: owner, RequireSignature: true,
})
```

Le service BEP autonome accepte ces membres dans sa configuration habituelle :

```json
{
  "author": {"name": "Project author", "email": "author@example.invalid", "timezone_offset": 0},
  "signing_key_file": "../private-keys/author.syrsig",
  "signing_key_password_env": "SYRNCK_SIGNING_KEY_PASSWORD"
}
```

C’est un extrait, pas une configuration complète. Chemins relatifs au fichier
de configuration. `signing_key_file` exige un auteur et exclut `signing_key_env`.
Sa variable de mot de passe doit être distincte des identifiants de stockage.
Le fichier doit être une enveloppe ordinaire de 104 octets sans lien, hors du
workspace ou dans son Store protégé. L’authentification précède les workers de
dossier et l’écoute TCP. Le service efface la variable de mot de passe après
authentification, conserve le propriétaire jusqu’à l’arrêt des workers et impose
la signature des nouvelles révisions. Redémarrer pour changer les identifiants.


Le CLI accepte aussi `SYRNCK_SIGNING_KEY_VAULT`, et le service autonome `signing_key_vault`, via le [workflow des coffres système](doc:system-vaults#services). Ces fournisseurs excluent les signataires bruts/fichiers, exigent un auteur et conservent des propriétaires natifs typés jusqu’à l’arrêt. Ils ne stockent pas les clés privées d’appareil TLS.

## Format et limites {#format}

L’enveloppe expérimentale de signature v1 utilise les octets magiques
`SYRSIG 01 00` puis les mêmes coûts/sel/nonce/payload chiffré de 32 octets/tag que
les [enveloppes de stockage](doc:api-password-keys#protection). L’en-tête complet
de 56 octets, destination comprise, est authentifié. Remplacer les octets de
destination ne convertit pas une enveloppe valide : l’authentification échoue.
Argon2id v1.3 utilise une voie ; chaque protection renouvelle sel de 16 octets
et nonce XChaCha20-Poly1305 de 24 octets.

KDF synchrone, sans annulation dans Argon2id. Propriétaires natifs et buffers KDF
mutables sont effacés ; mots de passe de l’appelant, chaînes d’environnement et
copies du runtime restent à sa charge. Aucune promesse de mémoire verrouillée,
coffre OS, révocation distribuée, rotation des clés ou chiffrement de bout en bout.
