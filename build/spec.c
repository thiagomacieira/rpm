#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include "spec.h"
#include "misc.h"
#include "rpmlib.h"
#include "package.h"
#include "read.h"
#include "files.h"
#include "macro.h"

static char *getSourceAux(Spec spec, int num, int flag, int full);
static struct Source *findSource(Spec spec, int num, int flag);

Spec newSpec(void)
{
    Spec spec;

    spec = (Spec)malloc(sizeof *spec);
    
    spec->specFile = NULL;
    spec->sourceRpmName = NULL;

    spec->file = NULL;
    spec->readBuf[0] = '\0';
    spec->readPtr = NULL;
    spec->line[0] = '\0';
    spec->readStack = malloc(sizeof(struct ReadLevelEntry));
    spec->readStack->next = NULL;
    spec->readStack->reading = 1;

    spec->prep = NULL;
    spec->build = NULL;
    spec->install = NULL;
    spec->clean = NULL;

    spec->sources = NULL;
    spec->packages = NULL;
    spec->noSource = 0;
    spec->numSources = 0;

    spec->sourceHeader = NULL;

    spec->sourceCpioCount = 0;
    spec->sourceCpioList = NULL;
    
    spec->gotBuildRoot = 0;
    spec->buildRoot = NULL;
    
    spec->buildSubdir = NULL;

    spec->docDir = NULL;

    spec->passPhrase = NULL;
    spec->timeCheck = 0;
    spec->cookie = NULL;

    spec->buildRestrictions = headerNew();
    spec->buildArchitectures = NULL;
    spec->buildArchitectureCount = 0;
    spec->inBuildArchitectures = 0;
    spec->buildArchitectureSpecs = NULL;

    initMacros(&spec->macros);
    
    spec->autoReq = 1;
    spec->autoProv = 1;

    return spec;
}

void freeSpec(Spec spec)
{
    struct ReadLevelEntry *rl;
    
    freeStringBuf(spec->prep);
    freeStringBuf(spec->build);
    freeStringBuf(spec->install);
    freeStringBuf(spec->clean);

    FREE(spec->buildRoot);
    FREE(spec->buildSubdir);
    FREE(spec->specFile);
    FREE(spec->sourceRpmName);
    FREE(spec->docDir);

    while (spec->readStack) {
	rl = spec->readStack;
	spec->readStack = spec->readStack->next;
	free(rl);
    }
    
    if (spec->sourceHeader) {
	headerFree(spec->sourceHeader);
    }

    freeCpioList(spec->sourceCpioList, spec->sourceCpioCount);
    
    headerFree(spec->buildRestrictions);
    FREE(spec->buildArchitectures);

    if (!spec->inBuildArchitectures) {
	while (spec->buildArchitectureCount--) {
	    freeSpec(
		spec->buildArchitectureSpecs[spec->buildArchitectureCount]);
	}
    }
    FREE(spec->buildArchitectures);

    FREE(spec->passPhrase);
    FREE(spec->cookie);

    freeMacros(&spec->macros);
    
    freeSources(spec);
    freePackages(spec);
    closeSpec(spec);
    
    free(spec);
}

int addSource(Spec spec, Package pkg, char *field, int tag)
{
    struct Source *p;
    int flag = 0;
    char *name = NULL;
    char *nump, *fieldp = NULL;
    char buf[BUFSIZ];
    char expansion[BUFSIZ];
    int num = 0;

    switch (tag) {
      case RPMTAG_SOURCE:
	flag = RPMBUILD_ISSOURCE;
	name = "source";
	fieldp = spec->line + 6;
	break;
      case RPMTAG_PATCH:
	flag = RPMBUILD_ISPATCH;
	name = "patch";
	fieldp = spec->line + 5;
	break;
      case RPMTAG_ICON:
	flag = RPMBUILD_ISICON;
	break;
    }

    /* Get the number */
    if (tag != RPMTAG_ICON) {
	/* We already know that a ':' exists, and that there */
	/* are no spaces before it.                          */

	nump = buf;
	while (*fieldp != ':') {
	    *nump++ = *fieldp++;
	}
	*nump = '\0';

	nump = buf;
	SKIPSPACE(nump);
	if (! *nump) {
	    num = 0;
	} else {
	    if (parseNum(buf, &num)) {
		rpmError(RPMERR_BADSPEC, "line %d: Bad %s number: %s\n",
			 spec->lineNum, name, spec->line);
		return RPMERR_BADSPEC;
	    }
	}
    }

    /* Create the entry and link it in */
    p = malloc(sizeof(struct Source));
    p->num = num;
    p->fullSource = strdup(field);
    p->source = strrchr(p->fullSource, '/');
    p->flags = flag;
    if (p->source) {
	p->source++;
    } else {
	p->source = p->fullSource;
    }

    if (tag != RPMTAG_ICON) {
	p->next = spec->sources;
	spec->sources = p;
    } else {
	p->next = pkg->icon;
	pkg->icon = p;
    }

    spec->numSources++;

    if (tag != RPMTAG_ICON) {
	sprintf(expansion, "%s/%s", rpmGetVar(RPMVAR_SOURCEDIR), p->source);
	sprintf(buf, "%s%d",
		(flag & RPMBUILD_ISPATCH) ? "PATCH" : "SOURCE", num);
	addMacro(&spec->macros, buf, expansion);
	sprintf(buf, "%sURL%d",
		(flag & RPMBUILD_ISPATCH) ? "PATCH" : "SOURCE", num);
	addMacro(&spec->macros, buf, p->fullSource);
    }
    
    return 0;
}

char *getSource(Spec spec, int num, int flag)
{
    return getSourceAux(spec, num, flag, 0);
}

char *getFullSource(Spec spec, int num, int flag)
{
    return getSourceAux(spec, num, flag, 1);
}

static char *getSourceAux(Spec spec, int num, int flag, int full)
{
    struct Source *p = spec->sources;

    p = findSource(spec, num, flag);

    return (p) ? (full ? p->fullSource : p->source) : NULL;
}

static struct Source *findSource(Spec spec, int num, int flag)
{
    struct Source *p = spec->sources;

    while (p) {
	if ((num == p->num) && (p->flags & flag)) {
	    return p;
	}
	p = p->next;
    }

    return NULL;
}

void freeSources(Spec spec)
{
    struct Source *p1, *p2;

    p1 = spec->sources;
    while (p1) {
	p2 = p1;
	p1 = p1->next;
	FREE(p2->fullSource);
	free(p2);
    }
}

int parseNoSource(Spec spec, char *field, int tag)
{
    char buf[BUFSIZ];
    char *s, *name;
    int num, flag;
    struct Source *p;

    if (tag == RPMTAG_NOSOURCE) {
	flag = RPMBUILD_ISSOURCE;
	name = "source";
    } else {
	flag = RPMBUILD_ISPATCH;
	name = "patch";
    }
    
    strcpy(buf, field);
    field = buf;
    while ((s = strtok(field, ", \t"))) {
	if (parseNum(s, &num)) {
	    rpmError(RPMERR_BADSPEC, "line %d: Bad number: %s",
		     spec->lineNum, spec->line);
	    return RPMERR_BADSPEC;
	}

	if (! (p = findSource(spec, num, flag))) {
	    rpmError(RPMERR_BADSPEC, "line %d: Bad no%s number: %d",
		     spec->lineNum, name, num);
	    return RPMERR_BADSPEC;
	}

	p->flags |= RPMBUILD_ISNO;

	field = NULL;
    }

    return 0;
}
