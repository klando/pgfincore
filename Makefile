PKGNAME = pgfincore
PKGVERS = $(shell dpkg-parsechangelog | awk -F '[:-]' '/^Version:/ { print substr($$2, 2) }')

DEBDIR = /tmp/$(PKGNAME)
EXPORT = $(DEBDIR)/export/$(PKGNAME)-$(PKGVERS)
ORIG   = $(DEBDIR)/export/$(PKGNAME)_$(PKGVERS).orig.tar.gz
ARCHIVE= $(DEBDIR)/export/$(PKGNAME)-$(PKGVERS).tar.gz
DEBEXTS= {gz,changes,build,dsc}

MODULES = pgfincore
DATA_built = pgfincore.sql uninstall_pgfincore.sql
DOCS = README.pgfincore

PG_CONFIG ?= pg_config
PGXS = $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

deb:
	# working copy from where to make the .orig archive
	rm -rf $(DEBDIR)	
	mkdir -p $(DEBDIR)/$(PKGNAME)-$(PKGVERS)
	mkdir -p $(EXPORT)
	rsync -Ca . $(EXPORT)

	# get rid of temp and build files
	for n in ".#*" "*~" "build-stamp" "configure-stamp" "prefix.sql" "prefix.so"; do \
	  find $(EXPORT) -name "$$n" -print0|xargs -0 rm -f; \
	done

	# prepare the .orig without the debian/ packaging stuff
	rsync -Ca $(EXPORT) $(DEBDIR)
	rm -rf $(DEBDIR)/$(PKGNAME)-$(PKGVERS)/debian
	(cd $(DEBDIR) && tar czf $(ORIG) $(PKGNAME)-$(PKGVERS))

	# have a copy of the $$ORIG file named $$ARCHIVE for non-debian packagers
	cp $(ORIG) $(ARCHIVE)

	# build the debian package and copy them to ..
	(cd $(EXPORT) && make -f debian/rules debian/control && debuild -us -uc)

	cp $(EXPORT)/debian/control debian
	find $(DEBDIR)/export -maxdepth 1 -type f -name "*$(PGGVERS)*" -exec cp {} .. \;

.PHONY: deb
