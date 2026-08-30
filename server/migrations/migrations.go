// Package migrations embeds the SQL schema migrations so the server binary
// is self-contained. Migration files are applied in lexical order at startup;
// never edit an applied migration — add a new file instead.
package migrations

import "embed"

//go:embed *.sql
var FS embed.FS
