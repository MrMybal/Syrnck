---
id: server-signatures
title: Signatures de révision autorisées
description: Lier clés de committer et comptes serveur, puis imposer une histoire signée.
section: Transferts
order: 515
parent: server-access
sourceRevision: 1
---

## Configurer les clés approuvées {#configuration}

Le schéma facultatif v2 du fichier d’accès ajoute l’autorisation des signatures
aux [comptes, permissions et verrous](doc:server-access). La v1 garde ses champs
et son comportement exacts. La v2 conserve les trois champs racine, mais chaque
principal ajoute `signing_keys` et chaque branche `require_signatures`.
Un ancien serveur refuse cette configuration.

```json
{
  "version": 2,
  "principals": [
    {
      "id": "alice",
      "token_hash": "REPLACE_WITH_TOKEN_HASH",
      "role": "writer",
      "write_paths": ["Source", "Content"],
      "branches": ["main"],
      "signing_keys": [
        {
          "public_key": "REPLACE_WITH_64_LOWERCASE_HEX_PUBLIC_KEY",
          "name": "Alice",
          "email": "alice@example.invalid"
        }
      ]
    }
  ],
  "branches": [
    {"name": "main", "fast_forward": true, "lock_paths": ["Content"], "require_signatures": true}
  ]
}
```

Remplacer le placeholder de jeton avec `token-hash`. Obtenir la clé publique via
`native identity` avec la clé de signature chargée, ou la signature d’une
révision vérifiée. Le fichier contient des clés publiques, jamais de graines.
Chaque liaison contient exactement `public_key,name,email` : clé Ed25519
canonique et identité UTF-8 exacte du committer autorisé. Limites de nom/adresse
identiques aux [identités natives](doc:native-authorship#identity). Fuseau et date
restent des déclarations, sans preuve du moment de signature.

Au plus 32 clés par principal. Une clé n’apparaît qu’une fois dans toute la
configuration, autres comptes compris. Clé mal formée, non canonique, hors courbe
ou de petit ordre : refus au démarrage. Reader exige une liste vide ; writer/admin
peut n’avoir aucune clé s’il transporte seulement le travail d’un autre committer
approuvé. Plafonds existants de 1 Mio et de comptes/branches conservés.

```text
syrnck --store server-store serve --access access-v2.json --require-lease
syrnck help server-signatures
```

## Valider chaque nouveau commit admis {#validation}

Sur une branche avec `require_signatures: true`, chaque révision nouvellement
atteignable doit être v2 avec une signature Ed25519 valide. Sa clé publique doit
être enregistrée et les nom/adresse du committer doivent correspondre exactement.
Le compte signataire doit actuellement pouvoir écrire la branche cible et chaque
chemin changé dans ce commit, comparaisons avec chaque parent de merge comprises.
Une tête correcte ne masque pas un commit intermédiaire ou second parent non
signé, à clé inconnue ou identité différente. Un contenu ajouté puis retiré avant
la tête reste contrôlé.

Le publieur authentifié exige séparément permissions de branche/chemins et
revendications de verrous valides. Publieur et signataire peuvent différer :
un collègue peut transporter l’histoire approuvée. Le rôle admin ne contourne pas
la politique de signature. Fast-forward, validation complète des dépendances,
contrôle final d’expiration des verrous et publication sous valeur attendue
restent applicables. Un refus conserve la référence publiée ; les objets déjà
envoyés suivent les règles ordinaires de transfert et GC.

Seuls les ancêtres de la tête publiée actuelle de cette branche constituent une
base déjà admise. Activer la politique sur une branche existante n’exige pas de
réécrire son histoire ancienne, mais chaque nouveau commit doit la respecter.
Une tête d’une autre branche n’admet pas cette histoire ici. Une publication
initiale sur branche vide exige donc toute l’histoire introduite conforme.

Un véritable retour forcé doit aussi cibler une tête signée actuellement approuvée,
même déjà dans la base ; aucun retour vers métadonnées anciennes, non signées ou
à clé révoquée. Une reprise strictement identique de la tête publiée peut terminer
un transfert après révocation : elle n’introduit aucune révision. Avec
`require_signatures: false`, règles usuelles de jeton/chemins/branches, tout en
vérifiant cryptographiquement les signatures présentes dans les objets.

## Renouveler et révoquer {#rotation}

Le fichier d’accès appartient à l’administration. Les modifications exigent un
redémarrage serveur, comme comptes et jetons. Pour renouveler, enregistrer la
nouvelle clé publique avec l’ancienne, distribuer la nouvelle clé privée par un
mécanisme protégé, puis retirer l’ancienne clé publique au moment choisi.
Retirer une liaison révoque les admissions futures avec cette clé. Retirer un
compte ou ses permissions de branche/chemins affecte aussi les publications futures.

L’histoire déjà admise reste lisible avec ses octets de signature d’origine.
Un nouvel enfant approuvé peut prolonger cette base après révocation d’une clé
ancienne. Réintroduire des commits à clé ancienne par une nouvelle histoire est
refusé. Rotation du jeton et rotation de signature sont indépendantes. La politique
utilise la configuration actuelle du démarrage, pas la date déclarée du commit.
Les requêtes engagées suivent cette configuration immuable jusqu’à leur fin.

## Frontières de confiance {#limits}

L’autorisation concerne le committer actuel et la révision complète signée.
L’auteur d’origine peut différer ou être null après réécriture ; son nom reste
une déclaration historique. Aucun horodatage fiable, validité historique de
certificat, flux distribué de révocation, rechargement à chaud ou coffre système
de clé de signature ici. Ni confidentialité de lecture par chemin ni chiffrement
de bout en bout : le serveur reçoit encore les objets décodés et exige le
déploiement HTTPS habituel.

## Vérification {#verification}

```text
python scripts/build.py build --target syrnck_server_signatures_tests syrnck
python scripts/build.py test -R "^server_(signatures|signatures_policy|access_policy)$"
```

Tests de schéma strict, clés/identités différentes, tous les nouveaux parents,
transport délégué, ACL séparées signataire/publieur, bases anciennes déjà admises,
révocation, rotation et retours interdits. Les tests HTTP authentifiés transfèrent
aussi une histoire signée via un Store chiffré vers un lecteur sans clé privée,
redémarrent avec de nouvelles politiques et auditent les objets conservés.
