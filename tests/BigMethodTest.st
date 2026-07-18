"=== Standard test harness. Paste verbatim at the top of each tests/*.st file.
 Every check is wrapped so an unexpected runtime error counts as a FAILURE
 (a raw Assert failure or uncaught error prints a backtrace yet still exits 0,
 i.e. a false pass). The final top-level block MUST end with ^t report, whose
 integer return becomes the process exit code (0 = pass) that run_tests.sh
 gates on. Do not use the old Assert class in new tests. All catches use
 Exception (the root) so a wrong error-class name surfaces as a visible FAIL,
 never a silent exit-0. ==="
TestRun := Object [
    | fails count name |
    class named: aString [ ^self new setName: aString ]
    setName: aString [ name := aString. fails := 0. count := 0. ^self ]
    fail: aLabel [
        fails := fails + 1.
        Transcript nextPutAll: name; nextPutAll: ' FAIL: '; nextPutAll: aLabel; lf.
    ]
    error: aLabel from: e [
        fails := fails + 1.
        Transcript nextPutAll: name; nextPutAll: ' ERROR ['; nextPutAll: aLabel;
            nextPutAll: ']: '; nextPutAll: e messageText; lf.
    ]
    check: aLabel that: aBlock [
        count := count + 1.
        [ (aBlock value = true) ifFalse: [ self fail: aLabel ] ]
            on: Exception do: [:e | self error: aLabel from: e ].
    ]
    check: aLabel equals: expected is: aBlock [
        count := count + 1.
        [ | got |
          got := aBlock value.
          got = expected ifFalse: [
              self fail: aLabel, ' (got ', got printString, ' expected ', expected printString, ')' ] ]
            on: Exception do: [:e | self error: aLabel from: e ].
    ]
    expect: aLabel raises: aClass in: aBlock [
        count := count + 1.
        [ aBlock value. self fail: aLabel, ' (no error raised)' ]
            on: Exception do: [:e |
                (aClass notNil and: [ e isKindOf: aClass ]) ifFalse: [
                    self fail: aLabel, ' (raised ', e class printString, ' not ', aClass printString, ')' ] ].
    ]
    report [
        Transcript nextPutAll: name; nextPutAll: ': ';
            nextPutAll: (count - fails) printString; nextPutAll: '/';
            nextPutAll: count printString; nextPutAll: ' ok'; lf.
        ^fails
    ]
]


"Fixture: giant compiled shapes that used to hit the fixed compile-time buffer
 web (pointersOffsets[1024] + icSites[1024] in the assembler, the 1024-handle
 scope cap, the register allocator's 256-var / 128-temp arrays). All of those
 are heap-sized per method now; this file is the regression gate. Every shape
 here must COMPILE and produce exact values. Shapes that exceed the remaining
 honest ceiling (255 uint8 frame slots in CompiledCodeHeader) die with a clean
 compile error, which cannot be tested in-process (the compiler FAILs), so only
 compilable shapes live here."

BigLit := Object [
    build [ | c |
        c := OrderedCollection new.
        c add: 'v0'.
        c add: 'v1'.
        c add: 'v2'.
        c add: 'v3'.
        c add: 'v4'.
        c add: 'v5'.
        c add: 'v6'.
        c add: 'v7'.
        c add: 'v8'.
        c add: 'v9'.
        c add: 'v10'.
        c add: 'v11'.
        c add: 'v12'.
        c add: 'v13'.
        c add: 'v14'.
        c add: 'v15'.
        c add: 'v16'.
        c add: 'v17'.
        c add: 'v18'.
        c add: 'v19'.
        c add: 'v20'.
        c add: 'v21'.
        c add: 'v22'.
        c add: 'v23'.
        c add: 'v24'.
        c add: 'v25'.
        c add: 'v26'.
        c add: 'v27'.
        c add: 'v28'.
        c add: 'v29'.
        c add: 'v30'.
        c add: 'v31'.
        c add: 'v32'.
        c add: 'v33'.
        c add: 'v34'.
        c add: 'v35'.
        c add: 'v36'.
        c add: 'v37'.
        c add: 'v38'.
        c add: 'v39'.
        c add: 'v40'.
        c add: 'v41'.
        c add: 'v42'.
        c add: 'v43'.
        c add: 'v44'.
        c add: 'v45'.
        c add: 'v46'.
        c add: 'v47'.
        c add: 'v48'.
        c add: 'v49'.
        c add: 'v50'.
        c add: 'v51'.
        c add: 'v52'.
        c add: 'v53'.
        c add: 'v54'.
        c add: 'v55'.
        c add: 'v56'.
        c add: 'v57'.
        c add: 'v58'.
        c add: 'v59'.
        c add: 'v60'.
        c add: 'v61'.
        c add: 'v62'.
        c add: 'v63'.
        c add: 'v64'.
        c add: 'v65'.
        c add: 'v66'.
        c add: 'v67'.
        c add: 'v68'.
        c add: 'v69'.
        c add: 'v70'.
        c add: 'v71'.
        c add: 'v72'.
        c add: 'v73'.
        c add: 'v74'.
        c add: 'v75'.
        c add: 'v76'.
        c add: 'v77'.
        c add: 'v78'.
        c add: 'v79'.
        c add: 'v80'.
        c add: 'v81'.
        c add: 'v82'.
        c add: 'v83'.
        c add: 'v84'.
        c add: 'v85'.
        c add: 'v86'.
        c add: 'v87'.
        c add: 'v88'.
        c add: 'v89'.
        c add: 'v90'.
        c add: 'v91'.
        c add: 'v92'.
        c add: 'v93'.
        c add: 'v94'.
        c add: 'v95'.
        c add: 'v96'.
        c add: 'v97'.
        c add: 'v98'.
        c add: 'v99'.
        c add: 'v100'.
        c add: 'v101'.
        c add: 'v102'.
        c add: 'v103'.
        c add: 'v104'.
        c add: 'v105'.
        c add: 'v106'.
        c add: 'v107'.
        c add: 'v108'.
        c add: 'v109'.
        c add: 'v110'.
        c add: 'v111'.
        c add: 'v112'.
        c add: 'v113'.
        c add: 'v114'.
        c add: 'v115'.
        c add: 'v116'.
        c add: 'v117'.
        c add: 'v118'.
        c add: 'v119'.
        c add: 'v120'.
        c add: 'v121'.
        c add: 'v122'.
        c add: 'v123'.
        c add: 'v124'.
        c add: 'v125'.
        c add: 'v126'.
        c add: 'v127'.
        c add: 'v128'.
        c add: 'v129'.
        c add: 'v130'.
        c add: 'v131'.
        c add: 'v132'.
        c add: 'v133'.
        c add: 'v134'.
        c add: 'v135'.
        c add: 'v136'.
        c add: 'v137'.
        c add: 'v138'.
        c add: 'v139'.
        c add: 'v140'.
        c add: 'v141'.
        c add: 'v142'.
        c add: 'v143'.
        c add: 'v144'.
        c add: 'v145'.
        c add: 'v146'.
        c add: 'v147'.
        c add: 'v148'.
        c add: 'v149'.
        c add: 'v150'.
        c add: 'v151'.
        c add: 'v152'.
        c add: 'v153'.
        c add: 'v154'.
        c add: 'v155'.
        c add: 'v156'.
        c add: 'v157'.
        c add: 'v158'.
        c add: 'v159'.
        c add: 'v160'.
        c add: 'v161'.
        c add: 'v162'.
        c add: 'v163'.
        c add: 'v164'.
        c add: 'v165'.
        c add: 'v166'.
        c add: 'v167'.
        c add: 'v168'.
        c add: 'v169'.
        c add: 'v170'.
        c add: 'v171'.
        c add: 'v172'.
        c add: 'v173'.
        c add: 'v174'.
        c add: 'v175'.
        c add: 'v176'.
        c add: 'v177'.
        c add: 'v178'.
        c add: 'v179'.
        c add: 'v180'.
        c add: 'v181'.
        c add: 'v182'.
        c add: 'v183'.
        c add: 'v184'.
        c add: 'v185'.
        c add: 'v186'.
        c add: 'v187'.
        c add: 'v188'.
        c add: 'v189'.
        c add: 'v190'.
        c add: 'v191'.
        c add: 'v192'.
        c add: 'v193'.
        c add: 'v194'.
        c add: 'v195'.
        c add: 'v196'.
        c add: 'v197'.
        c add: 'v198'.
        c add: 'v199'.
        c add: 'v200'.
        c add: 'v201'.
        c add: 'v202'.
        c add: 'v203'.
        c add: 'v204'.
        c add: 'v205'.
        c add: 'v206'.
        c add: 'v207'.
        c add: 'v208'.
        c add: 'v209'.
        c add: 'v210'.
        c add: 'v211'.
        c add: 'v212'.
        c add: 'v213'.
        c add: 'v214'.
        c add: 'v215'.
        c add: 'v216'.
        c add: 'v217'.
        c add: 'v218'.
        c add: 'v219'.
        c add: 'v220'.
        c add: 'v221'.
        c add: 'v222'.
        c add: 'v223'.
        c add: 'v224'.
        c add: 'v225'.
        c add: 'v226'.
        c add: 'v227'.
        c add: 'v228'.
        c add: 'v229'.
        c add: 'v230'.
        c add: 'v231'.
        c add: 'v232'.
        c add: 'v233'.
        c add: 'v234'.
        c add: 'v235'.
        c add: 'v236'.
        c add: 'v237'.
        c add: 'v238'.
        c add: 'v239'.
        c add: 'v240'.
        c add: 'v241'.
        c add: 'v242'.
        c add: 'v243'.
        c add: 'v244'.
        c add: 'v245'.
        c add: 'v246'.
        c add: 'v247'.
        c add: 'v248'.
        c add: 'v249'.
        c add: 'v250'.
        c add: 'v251'.
        c add: 'v252'.
        c add: 'v253'.
        c add: 'v254'.
        c add: 'v255'.
        c add: 'v256'.
        c add: 'v257'.
        c add: 'v258'.
        c add: 'v259'.
        c add: 'v260'.
        c add: 'v261'.
        c add: 'v262'.
        c add: 'v263'.
        c add: 'v264'.
        c add: 'v265'.
        c add: 'v266'.
        c add: 'v267'.
        c add: 'v268'.
        c add: 'v269'.
        c add: 'v270'.
        c add: 'v271'.
        c add: 'v272'.
        c add: 'v273'.
        c add: 'v274'.
        c add: 'v275'.
        c add: 'v276'.
        c add: 'v277'.
        c add: 'v278'.
        c add: 'v279'.
        c add: 'v280'.
        c add: 'v281'.
        c add: 'v282'.
        c add: 'v283'.
        c add: 'v284'.
        c add: 'v285'.
        c add: 'v286'.
        c add: 'v287'.
        c add: 'v288'.
        c add: 'v289'.
        c add: 'v290'.
        c add: 'v291'.
        c add: 'v292'.
        c add: 'v293'.
        c add: 'v294'.
        c add: 'v295'.
        c add: 'v296'.
        c add: 'v297'.
        c add: 'v298'.
        c add: 'v299'.
        c add: 'v300'.
        c add: 'v301'.
        c add: 'v302'.
        c add: 'v303'.
        c add: 'v304'.
        c add: 'v305'.
        c add: 'v306'.
        c add: 'v307'.
        c add: 'v308'.
        c add: 'v309'.
        c add: 'v310'.
        c add: 'v311'.
        c add: 'v312'.
        c add: 'v313'.
        c add: 'v314'.
        c add: 'v315'.
        c add: 'v316'.
        c add: 'v317'.
        c add: 'v318'.
        c add: 'v319'.
        c add: 'v320'.
        c add: 'v321'.
        c add: 'v322'.
        c add: 'v323'.
        c add: 'v324'.
        c add: 'v325'.
        c add: 'v326'.
        c add: 'v327'.
        c add: 'v328'.
        c add: 'v329'.
        c add: 'v330'.
        c add: 'v331'.
        c add: 'v332'.
        c add: 'v333'.
        c add: 'v334'.
        c add: 'v335'.
        c add: 'v336'.
        c add: 'v337'.
        c add: 'v338'.
        c add: 'v339'.
        c add: 'v340'.
        c add: 'v341'.
        c add: 'v342'.
        c add: 'v343'.
        c add: 'v344'.
        c add: 'v345'.
        c add: 'v346'.
        c add: 'v347'.
        c add: 'v348'.
        c add: 'v349'.
        c add: 'v350'.
        c add: 'v351'.
        c add: 'v352'.
        c add: 'v353'.
        c add: 'v354'.
        c add: 'v355'.
        c add: 'v356'.
        c add: 'v357'.
        c add: 'v358'.
        c add: 'v359'.
        c add: 'v360'.
        c add: 'v361'.
        c add: 'v362'.
        c add: 'v363'.
        c add: 'v364'.
        c add: 'v365'.
        c add: 'v366'.
        c add: 'v367'.
        c add: 'v368'.
        c add: 'v369'.
        c add: 'v370'.
        c add: 'v371'.
        c add: 'v372'.
        c add: 'v373'.
        c add: 'v374'.
        c add: 'v375'.
        c add: 'v376'.
        c add: 'v377'.
        c add: 'v378'.
        c add: 'v379'.
        c add: 'v380'.
        c add: 'v381'.
        c add: 'v382'.
        c add: 'v383'.
        c add: 'v384'.
        c add: 'v385'.
        c add: 'v386'.
        c add: 'v387'.
        c add: 'v388'.
        c add: 'v389'.
        c add: 'v390'.
        c add: 'v391'.
        c add: 'v392'.
        c add: 'v393'.
        c add: 'v394'.
        c add: 'v395'.
        c add: 'v396'.
        c add: 'v397'.
        c add: 'v398'.
        c add: 'v399'.
        c add: 'v400'.
        c add: 'v401'.
        c add: 'v402'.
        c add: 'v403'.
        c add: 'v404'.
        c add: 'v405'.
        c add: 'v406'.
        c add: 'v407'.
        c add: 'v408'.
        c add: 'v409'.
        c add: 'v410'.
        c add: 'v411'.
        c add: 'v412'.
        c add: 'v413'.
        c add: 'v414'.
        c add: 'v415'.
        c add: 'v416'.
        c add: 'v417'.
        c add: 'v418'.
        c add: 'v419'.
        c add: 'v420'.
        c add: 'v421'.
        c add: 'v422'.
        c add: 'v423'.
        c add: 'v424'.
        c add: 'v425'.
        c add: 'v426'.
        c add: 'v427'.
        c add: 'v428'.
        c add: 'v429'.
        c add: 'v430'.
        c add: 'v431'.
        c add: 'v432'.
        c add: 'v433'.
        c add: 'v434'.
        c add: 'v435'.
        c add: 'v436'.
        c add: 'v437'.
        c add: 'v438'.
        c add: 'v439'.
        c add: 'v440'.
        c add: 'v441'.
        c add: 'v442'.
        c add: 'v443'.
        c add: 'v444'.
        c add: 'v445'.
        c add: 'v446'.
        c add: 'v447'.
        c add: 'v448'.
        c add: 'v449'.
        c add: 'v450'.
        c add: 'v451'.
        c add: 'v452'.
        c add: 'v453'.
        c add: 'v454'.
        c add: 'v455'.
        c add: 'v456'.
        c add: 'v457'.
        c add: 'v458'.
        c add: 'v459'.
        c add: 'v460'.
        c add: 'v461'.
        c add: 'v462'.
        c add: 'v463'.
        c add: 'v464'.
        c add: 'v465'.
        c add: 'v466'.
        c add: 'v467'.
        c add: 'v468'.
        c add: 'v469'.
        c add: 'v470'.
        c add: 'v471'.
        c add: 'v472'.
        c add: 'v473'.
        c add: 'v474'.
        c add: 'v475'.
        c add: 'v476'.
        c add: 'v477'.
        c add: 'v478'.
        c add: 'v479'.
        c add: 'v480'.
        c add: 'v481'.
        c add: 'v482'.
        c add: 'v483'.
        c add: 'v484'.
        c add: 'v485'.
        c add: 'v486'.
        c add: 'v487'.
        c add: 'v488'.
        c add: 'v489'.
        c add: 'v490'.
        c add: 'v491'.
        c add: 'v492'.
        c add: 'v493'.
        c add: 'v494'.
        c add: 'v495'.
        c add: 'v496'.
        c add: 'v497'.
        c add: 'v498'.
        c add: 'v499'.
        c add: 'v500'.
        c add: 'v501'.
        c add: 'v502'.
        c add: 'v503'.
        c add: 'v504'.
        c add: 'v505'.
        c add: 'v506'.
        c add: 'v507'.
        c add: 'v508'.
        c add: 'v509'.
        c add: 'v510'.
        c add: 'v511'.
        c add: 'v512'.
        c add: 'v513'.
        c add: 'v514'.
        c add: 'v515'.
        c add: 'v516'.
        c add: 'v517'.
        c add: 'v518'.
        c add: 'v519'.
        c add: 'v520'.
        c add: 'v521'.
        c add: 'v522'.
        c add: 'v523'.
        c add: 'v524'.
        c add: 'v525'.
        c add: 'v526'.
        c add: 'v527'.
        c add: 'v528'.
        c add: 'v529'.
        c add: 'v530'.
        c add: 'v531'.
        c add: 'v532'.
        c add: 'v533'.
        c add: 'v534'.
        c add: 'v535'.
        c add: 'v536'.
        c add: 'v537'.
        c add: 'v538'.
        c add: 'v539'.
        c add: 'v540'.
        c add: 'v541'.
        c add: 'v542'.
        c add: 'v543'.
        c add: 'v544'.
        c add: 'v545'.
        c add: 'v546'.
        c add: 'v547'.
        c add: 'v548'.
        c add: 'v549'.
        c add: 'v550'.
        c add: 'v551'.
        c add: 'v552'.
        c add: 'v553'.
        c add: 'v554'.
        c add: 'v555'.
        c add: 'v556'.
        c add: 'v557'.
        c add: 'v558'.
        c add: 'v559'.
        c add: 'v560'.
        c add: 'v561'.
        c add: 'v562'.
        c add: 'v563'.
        c add: 'v564'.
        c add: 'v565'.
        c add: 'v566'.
        c add: 'v567'.
        c add: 'v568'.
        c add: 'v569'.
        c add: 'v570'.
        c add: 'v571'.
        c add: 'v572'.
        c add: 'v573'.
        c add: 'v574'.
        c add: 'v575'.
        c add: 'v576'.
        c add: 'v577'.
        c add: 'v578'.
        c add: 'v579'.
        c add: 'v580'.
        c add: 'v581'.
        c add: 'v582'.
        c add: 'v583'.
        c add: 'v584'.
        c add: 'v585'.
        c add: 'v586'.
        c add: 'v587'.
        c add: 'v588'.
        c add: 'v589'.
        c add: 'v590'.
        c add: 'v591'.
        c add: 'v592'.
        c add: 'v593'.
        c add: 'v594'.
        c add: 'v595'.
        c add: 'v596'.
        c add: 'v597'.
        c add: 'v598'.
        c add: 'v599'.
        c add: 'v600'.
        c add: 'v601'.
        c add: 'v602'.
        c add: 'v603'.
        c add: 'v604'.
        c add: 'v605'.
        c add: 'v606'.
        c add: 'v607'.
        c add: 'v608'.
        c add: 'v609'.
        c add: 'v610'.
        c add: 'v611'.
        c add: 'v612'.
        c add: 'v613'.
        c add: 'v614'.
        c add: 'v615'.
        c add: 'v616'.
        c add: 'v617'.
        c add: 'v618'.
        c add: 'v619'.
        c add: 'v620'.
        c add: 'v621'.
        c add: 'v622'.
        c add: 'v623'.
        c add: 'v624'.
        c add: 'v625'.
        c add: 'v626'.
        c add: 'v627'.
        c add: 'v628'.
        c add: 'v629'.
        c add: 'v630'.
        c add: 'v631'.
        c add: 'v632'.
        c add: 'v633'.
        c add: 'v634'.
        c add: 'v635'.
        c add: 'v636'.
        c add: 'v637'.
        c add: 'v638'.
        c add: 'v639'.
        c add: 'v640'.
        c add: 'v641'.
        c add: 'v642'.
        c add: 'v643'.
        c add: 'v644'.
        c add: 'v645'.
        c add: 'v646'.
        c add: 'v647'.
        c add: 'v648'.
        c add: 'v649'.
        c add: 'v650'.
        c add: 'v651'.
        c add: 'v652'.
        c add: 'v653'.
        c add: 'v654'.
        c add: 'v655'.
        c add: 'v656'.
        c add: 'v657'.
        c add: 'v658'.
        c add: 'v659'.
        c add: 'v660'.
        c add: 'v661'.
        c add: 'v662'.
        c add: 'v663'.
        c add: 'v664'.
        c add: 'v665'.
        c add: 'v666'.
        c add: 'v667'.
        c add: 'v668'.
        c add: 'v669'.
        c add: 'v670'.
        c add: 'v671'.
        c add: 'v672'.
        c add: 'v673'.
        c add: 'v674'.
        c add: 'v675'.
        c add: 'v676'.
        c add: 'v677'.
        c add: 'v678'.
        c add: 'v679'.
        c add: 'v680'.
        c add: 'v681'.
        c add: 'v682'.
        c add: 'v683'.
        c add: 'v684'.
        c add: 'v685'.
        c add: 'v686'.
        c add: 'v687'.
        c add: 'v688'.
        c add: 'v689'.
        c add: 'v690'.
        c add: 'v691'.
        c add: 'v692'.
        c add: 'v693'.
        c add: 'v694'.
        c add: 'v695'.
        c add: 'v696'.
        c add: 'v697'.
        c add: 'v698'.
        c add: 'v699'.
        ^c ]
]

BigTemps := Object [
    calc [ | acc t0 t1 t2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 t13 t14 t15 t16 t17 t18 t19 t20 t21 t22 t23 t24 t25 t26 t27 t28 t29 t30 t31 t32 t33 t34 t35 t36 t37 t38 t39 t40 t41 t42 t43 t44 t45 t46 t47 t48 t49 t50 t51 t52 t53 t54 t55 t56 t57 t58 t59 t60 t61 t62 t63 t64 t65 t66 t67 t68 t69 t70 t71 t72 t73 t74 t75 t76 t77 t78 t79 t80 t81 t82 t83 t84 t85 t86 t87 t88 t89 t90 t91 t92 t93 t94 t95 t96 t97 t98 t99 t100 t101 t102 t103 t104 t105 t106 t107 t108 t109 t110 t111 t112 t113 t114 t115 t116 t117 t118 t119 t120 t121 t122 t123 t124 t125 t126 t127 t128 t129 t130 t131 t132 t133 t134 t135 t136 t137 t138 t139 t140 t141 t142 t143 t144 t145 t146 t147 t148 t149 |
        t0 := 0.
        t1 := 3.
        t2 := 6.
        t3 := 9.
        t4 := 12.
        t5 := 15.
        t6 := 18.
        t7 := 21.
        t8 := 24.
        t9 := 27.
        t10 := 30.
        t11 := 33.
        t12 := 36.
        t13 := 39.
        t14 := 42.
        t15 := 45.
        t16 := 48.
        t17 := 51.
        t18 := 54.
        t19 := 57.
        t20 := 60.
        t21 := 63.
        t22 := 66.
        t23 := 69.
        t24 := 72.
        t25 := 75.
        t26 := 78.
        t27 := 81.
        t28 := 84.
        t29 := 87.
        t30 := 90.
        t31 := 93.
        t32 := 96.
        t33 := 99.
        t34 := 102.
        t35 := 105.
        t36 := 108.
        t37 := 111.
        t38 := 114.
        t39 := 117.
        t40 := 120.
        t41 := 123.
        t42 := 126.
        t43 := 129.
        t44 := 132.
        t45 := 135.
        t46 := 138.
        t47 := 141.
        t48 := 144.
        t49 := 147.
        t50 := 150.
        t51 := 153.
        t52 := 156.
        t53 := 159.
        t54 := 162.
        t55 := 165.
        t56 := 168.
        t57 := 171.
        t58 := 174.
        t59 := 177.
        t60 := 180.
        t61 := 183.
        t62 := 186.
        t63 := 189.
        t64 := 192.
        t65 := 195.
        t66 := 198.
        t67 := 201.
        t68 := 204.
        t69 := 207.
        t70 := 210.
        t71 := 213.
        t72 := 216.
        t73 := 219.
        t74 := 222.
        t75 := 225.
        t76 := 228.
        t77 := 231.
        t78 := 234.
        t79 := 237.
        t80 := 240.
        t81 := 243.
        t82 := 246.
        t83 := 249.
        t84 := 252.
        t85 := 255.
        t86 := 258.
        t87 := 261.
        t88 := 264.
        t89 := 267.
        t90 := 270.
        t91 := 273.
        t92 := 276.
        t93 := 279.
        t94 := 282.
        t95 := 285.
        t96 := 288.
        t97 := 291.
        t98 := 294.
        t99 := 297.
        t100 := 300.
        t101 := 303.
        t102 := 306.
        t103 := 309.
        t104 := 312.
        t105 := 315.
        t106 := 318.
        t107 := 321.
        t108 := 324.
        t109 := 327.
        t110 := 330.
        t111 := 333.
        t112 := 336.
        t113 := 339.
        t114 := 342.
        t115 := 345.
        t116 := 348.
        t117 := 351.
        t118 := 354.
        t119 := 357.
        t120 := 360.
        t121 := 363.
        t122 := 366.
        t123 := 369.
        t124 := 372.
        t125 := 375.
        t126 := 378.
        t127 := 381.
        t128 := 384.
        t129 := 387.
        t130 := 390.
        t131 := 393.
        t132 := 396.
        t133 := 399.
        t134 := 402.
        t135 := 405.
        t136 := 408.
        t137 := 411.
        t138 := 414.
        t139 := 417.
        t140 := 420.
        t141 := 423.
        t142 := 426.
        t143 := 429.
        t144 := 432.
        t145 := 435.
        t146 := 438.
        t147 := 441.
        t148 := 444.
        t149 := 447.
        acc := 0.
        acc := acc + t0.
        acc := acc + t1.
        acc := acc + t2.
        acc := acc + t3.
        acc := acc + t4.
        acc := acc + t5.
        acc := acc + t6.
        acc := acc + t7.
        acc := acc + t8.
        acc := acc + t9.
        acc := acc + t10.
        acc := acc + t11.
        acc := acc + t12.
        acc := acc + t13.
        acc := acc + t14.
        acc := acc + t15.
        acc := acc + t16.
        acc := acc + t17.
        acc := acc + t18.
        acc := acc + t19.
        acc := acc + t20.
        acc := acc + t21.
        acc := acc + t22.
        acc := acc + t23.
        acc := acc + t24.
        acc := acc + t25.
        acc := acc + t26.
        acc := acc + t27.
        acc := acc + t28.
        acc := acc + t29.
        acc := acc + t30.
        acc := acc + t31.
        acc := acc + t32.
        acc := acc + t33.
        acc := acc + t34.
        acc := acc + t35.
        acc := acc + t36.
        acc := acc + t37.
        acc := acc + t38.
        acc := acc + t39.
        acc := acc + t40.
        acc := acc + t41.
        acc := acc + t42.
        acc := acc + t43.
        acc := acc + t44.
        acc := acc + t45.
        acc := acc + t46.
        acc := acc + t47.
        acc := acc + t48.
        acc := acc + t49.
        acc := acc + t50.
        acc := acc + t51.
        acc := acc + t52.
        acc := acc + t53.
        acc := acc + t54.
        acc := acc + t55.
        acc := acc + t56.
        acc := acc + t57.
        acc := acc + t58.
        acc := acc + t59.
        acc := acc + t60.
        acc := acc + t61.
        acc := acc + t62.
        acc := acc + t63.
        acc := acc + t64.
        acc := acc + t65.
        acc := acc + t66.
        acc := acc + t67.
        acc := acc + t68.
        acc := acc + t69.
        acc := acc + t70.
        acc := acc + t71.
        acc := acc + t72.
        acc := acc + t73.
        acc := acc + t74.
        acc := acc + t75.
        acc := acc + t76.
        acc := acc + t77.
        acc := acc + t78.
        acc := acc + t79.
        acc := acc + t80.
        acc := acc + t81.
        acc := acc + t82.
        acc := acc + t83.
        acc := acc + t84.
        acc := acc + t85.
        acc := acc + t86.
        acc := acc + t87.
        acc := acc + t88.
        acc := acc + t89.
        acc := acc + t90.
        acc := acc + t91.
        acc := acc + t92.
        acc := acc + t93.
        acc := acc + t94.
        acc := acc + t95.
        acc := acc + t96.
        acc := acc + t97.
        acc := acc + t98.
        acc := acc + t99.
        acc := acc + t100.
        acc := acc + t101.
        acc := acc + t102.
        acc := acc + t103.
        acc := acc + t104.
        acc := acc + t105.
        acc := acc + t106.
        acc := acc + t107.
        acc := acc + t108.
        acc := acc + t109.
        acc := acc + t110.
        acc := acc + t111.
        acc := acc + t112.
        acc := acc + t113.
        acc := acc + t114.
        acc := acc + t115.
        acc := acc + t116.
        acc := acc + t117.
        acc := acc + t118.
        acc := acc + t119.
        acc := acc + t120.
        acc := acc + t121.
        acc := acc + t122.
        acc := acc + t123.
        acc := acc + t124.
        acc := acc + t125.
        acc := acc + t126.
        acc := acc + t127.
        acc := acc + t128.
        acc := acc + t129.
        acc := acc + t130.
        acc := acc + t131.
        acc := acc + t132.
        acc := acc + t133.
        acc := acc + t134.
        acc := acc + t135.
        acc := acc + t136.
        acc := acc + t137.
        acc := acc + t138.
        acc := acc + t139.
        acc := acc + t140.
        acc := acc + t141.
        acc := acc + t142.
        acc := acc + t143.
        acc := acc + t144.
        acc := acc + t145.
        acc := acc + t146.
        acc := acc + t147.
        acc := acc + t148.
        acc := acc + t149.
        ^acc ]
]

BigVars := Object [
    calc [ | acc u0 u1 u2 u3 u4 u5 u6 u7 u8 u9 u10 u11 u12 u13 u14 u15 u16 u17 u18 u19 u20 u21 u22 u23 u24 u25 u26 u27 u28 u29 u30 u31 u32 u33 u34 u35 u36 u37 u38 u39 u40 u41 u42 u43 u44 u45 u46 u47 u48 u49 u50 u51 u52 u53 u54 u55 u56 u57 u58 u59 u60 u61 u62 u63 u64 u65 u66 u67 u68 u69 u70 u71 u72 u73 u74 u75 u76 u77 u78 u79 u80 u81 u82 u83 u84 u85 u86 u87 u88 u89 u90 u91 u92 u93 u94 u95 u96 u97 u98 u99 u100 u101 u102 u103 u104 u105 u106 u107 u108 u109 u110 u111 u112 u113 u114 u115 u116 u117 u118 u119 u120 u121 u122 u123 u124 u125 u126 u127 u128 u129 u130 u131 u132 u133 u134 u135 u136 u137 u138 u139 u140 u141 u142 u143 u144 u145 u146 u147 u148 u149 |
        u0 := 0.
        u1 := 1.
        u2 := 2.
        u3 := 3.
        u4 := 4.
        u5 := 5.
        u6 := 6.
        u7 := 7.
        u8 := 8.
        u9 := 9.
        u10 := 10.
        u11 := 11.
        u12 := 12.
        u13 := 13.
        u14 := 14.
        u15 := 15.
        u16 := 16.
        u17 := 17.
        u18 := 18.
        u19 := 19.
        u20 := 20.
        u21 := 21.
        u22 := 22.
        u23 := 23.
        u24 := 24.
        u25 := 25.
        u26 := 26.
        u27 := 27.
        u28 := 28.
        u29 := 29.
        u30 := 30.
        u31 := 31.
        u32 := 32.
        u33 := 33.
        u34 := 34.
        u35 := 35.
        u36 := 36.
        u37 := 37.
        u38 := 38.
        u39 := 39.
        u40 := 40.
        u41 := 41.
        u42 := 42.
        u43 := 43.
        u44 := 44.
        u45 := 45.
        u46 := 46.
        u47 := 47.
        u48 := 48.
        u49 := 49.
        u50 := 50.
        u51 := 51.
        u52 := 52.
        u53 := 53.
        u54 := 54.
        u55 := 55.
        u56 := 56.
        u57 := 57.
        u58 := 58.
        u59 := 59.
        u60 := 60.
        u61 := 61.
        u62 := 62.
        u63 := 63.
        u64 := 64.
        u65 := 65.
        u66 := 66.
        u67 := 67.
        u68 := 68.
        u69 := 69.
        u70 := 70.
        u71 := 71.
        u72 := 72.
        u73 := 73.
        u74 := 74.
        u75 := 75.
        u76 := 76.
        u77 := 77.
        u78 := 78.
        u79 := 79.
        u80 := 80.
        u81 := 81.
        u82 := 82.
        u83 := 83.
        u84 := 84.
        u85 := 85.
        u86 := 86.
        u87 := 87.
        u88 := 88.
        u89 := 89.
        u90 := 90.
        u91 := 91.
        u92 := 92.
        u93 := 93.
        u94 := 94.
        u95 := 95.
        u96 := 96.
        u97 := 97.
        u98 := 98.
        u99 := 99.
        u100 := 100.
        u101 := 101.
        u102 := 102.
        u103 := 103.
        u104 := 104.
        u105 := 105.
        u106 := 106.
        u107 := 107.
        u108 := 108.
        u109 := 109.
        u110 := 110.
        u111 := 111.
        u112 := 112.
        u113 := 113.
        u114 := 114.
        u115 := 115.
        u116 := 116.
        u117 := 117.
        u118 := 118.
        u119 := 119.
        u120 := 120.
        u121 := 121.
        u122 := 122.
        u123 := 123.
        u124 := 124.
        u125 := 125.
        u126 := 126.
        u127 := 127.
        u128 := 128.
        u129 := 129.
        u130 := 130.
        u131 := 131.
        u132 := 132.
        u133 := 133.
        u134 := 134.
        u135 := 135.
        u136 := 136.
        u137 := 137.
        u138 := 138.
        u139 := 139.
        u140 := 140.
        u141 := 141.
        u142 := 142.
        u143 := 143.
        u144 := 144.
        u145 := 145.
        u146 := 146.
        u147 := 147.
        u148 := 148.
        u149 := 149.
        acc := 0.
        acc := acc + u0.
        acc := acc + u1.
        acc := acc + u2.
        acc := acc + u3.
        acc := acc + u4.
        acc := acc + u5.
        acc := acc + u6.
        acc := acc + u7.
        acc := acc + u8.
        acc := acc + u9.
        acc := acc + u10.
        acc := acc + u11.
        acc := acc + u12.
        acc := acc + u13.
        acc := acc + u14.
        acc := acc + u15.
        acc := acc + u16.
        acc := acc + u17.
        acc := acc + u18.
        acc := acc + u19.
        acc := acc + u20.
        acc := acc + u21.
        acc := acc + u22.
        acc := acc + u23.
        acc := acc + u24.
        acc := acc + u25.
        acc := acc + u26.
        acc := acc + u27.
        acc := acc + u28.
        acc := acc + u29.
        acc := acc + u30.
        acc := acc + u31.
        acc := acc + u32.
        acc := acc + u33.
        acc := acc + u34.
        acc := acc + u35.
        acc := acc + u36.
        acc := acc + u37.
        acc := acc + u38.
        acc := acc + u39.
        acc := acc + u40.
        acc := acc + u41.
        acc := acc + u42.
        acc := acc + u43.
        acc := acc + u44.
        acc := acc + u45.
        acc := acc + u46.
        acc := acc + u47.
        acc := acc + u48.
        acc := acc + u49.
        acc := acc + u50.
        acc := acc + u51.
        acc := acc + u52.
        acc := acc + u53.
        acc := acc + u54.
        acc := acc + u55.
        acc := acc + u56.
        acc := acc + u57.
        acc := acc + u58.
        acc := acc + u59.
        acc := acc + u60.
        acc := acc + u61.
        acc := acc + u62.
        acc := acc + u63.
        acc := acc + u64.
        acc := acc + u65.
        acc := acc + u66.
        acc := acc + u67.
        acc := acc + u68.
        acc := acc + u69.
        acc := acc + u70.
        acc := acc + u71.
        acc := acc + u72.
        acc := acc + u73.
        acc := acc + u74.
        acc := acc + u75.
        acc := acc + u76.
        acc := acc + u77.
        acc := acc + u78.
        acc := acc + u79.
        acc := acc + u80.
        acc := acc + u81.
        acc := acc + u82.
        acc := acc + u83.
        acc := acc + u84.
        acc := acc + u85.
        acc := acc + u86.
        acc := acc + u87.
        acc := acc + u88.
        acc := acc + u89.
        acc := acc + u90.
        acc := acc + u91.
        acc := acc + u92.
        acc := acc + u93.
        acc := acc + u94.
        acc := acc + u95.
        acc := acc + u96.
        acc := acc + u97.
        acc := acc + u98.
        acc := acc + u99.
        acc := acc + u100.
        acc := acc + u101.
        acc := acc + u102.
        acc := acc + u103.
        acc := acc + u104.
        acc := acc + u105.
        acc := acc + u106.
        acc := acc + u107.
        acc := acc + u108.
        acc := acc + u109.
        acc := acc + u110.
        acc := acc + u111.
        acc := acc + u112.
        acc := acc + u113.
        acc := acc + u114.
        acc := acc + u115.
        acc := acc + u116.
        acc := acc + u117.
        acc := acc + u118.
        acc := acc + u119.
        acc := acc + u120.
        acc := acc + u121.
        acc := acc + u122.
        acc := acc + u123.
        acc := acc + u124.
        acc := acc + u125.
        acc := acc + u126.
        acc := acc + u127.
        acc := acc + u128.
        acc := acc + u129.
        acc := acc + u130.
        acc := acc + u131.
        acc := acc + u132.
        acc := acc + u133.
        acc := acc + u134.
        acc := acc + u135.
        acc := acc + u136.
        acc := acc + u137.
        acc := acc + u138.
        acc := acc + u139.
        acc := acc + u140.
        acc := acc + u141.
        acc := acc + u142.
        acc := acc + u143.
        acc := acc + u144.
        acc := acc + u145.
        acc := acc + u146.
        acc := acc + u147.
        acc := acc + u148.
        acc := acc + u149.
        u0 := Object name size.
        u1 := Array name size.
        u2 := String name size.
        u3 := Symbol name size.
        u4 := Character name size.
        u5 := Integer name size.
        u6 := SmallInteger name size.
        u7 := Float name size.
        u8 := Fraction name size.
        u9 := Number name size.
        u10 := Magnitude name size.
        u11 := Boolean name size.
        u12 := True name size.
        u13 := False name size.
        u14 := UndefinedObject name size.
        u15 := Collection name size.
        u16 := OrderedCollection name size.
        u17 := Dictionary name size.
        u18 := Set name size.
        u19 := Bag name size.
        u20 := Association name size.
        u21 := Interval name size.
        u22 := ByteArray name size.
        u23 := Stream name size.
        u24 := Exception name size.
        u25 := Error name size.
        u26 := Warning name size.
        u27 := Block name size.
        u28 := Message name size.
        u29 := MetaClass name size.
        acc := acc + u0.
        acc := acc + u1.
        acc := acc + u2.
        acc := acc + u3.
        acc := acc + u4.
        acc := acc + u5.
        acc := acc + u6.
        acc := acc + u7.
        acc := acc + u8.
        acc := acc + u9.
        acc := acc + u10.
        acc := acc + u11.
        acc := acc + u12.
        acc := acc + u13.
        acc := acc + u14.
        acc := acc + u15.
        acc := acc + u16.
        acc := acc + u17.
        acc := acc + u18.
        acc := acc + u19.
        acc := acc + u20.
        acc := acc + u21.
        acc := acc + u22.
        acc := acc + u23.
        acc := acc + u24.
        acc := acc + u25.
        acc := acc + u26.
        acc := acc + u27.
        acc := acc + u28.
        acc := acc + u29.
        ^acc ]
]

BigComma := Object [
    build [ ^'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' , 'ab' ]
]

[
    | t r s |
    t := TestRun named: 'BigMethod'.

    "--- 700 distinct literals in one method (old ceiling was ~290 sends) ---"
    r := BigLit new build.
    t check: 'BigLit size' equals: 700 is: [ r size ].
    t check: 'BigLit first' equals: 'v0' is: [ r first ].
    t check: 'BigLit middle' equals: 'v349' is: [ r at: 350 ].
    t check: 'BigLit last' equals: 'v699' is: [ r last ].
    t check: 'BigLit all distinct and ordered' that: [
        | ok i |
        ok := true. i := 1.
        r do: [:e | (e = ('v', (i - 1) printString)) ifFalse: [ ok := false ]. i := i + 1 ].
        ok ].

    "--- 150 temps, exact arithmetic (old allocator assert stopped at 128) ---"
    t check: 'BigTemps sum' equals: 33525 is: [ BigTemps new calc ].

    "--- temps + 30 association vars: total allocator vars cross the old 256 cap ---"
    t check: 'BigVars sum' equals: 11404 is: [ BigVars new calc ].

    "--- deep comma chain: 100-element AST spine ---"
    s := BigComma new build.
    t check: 'BigComma size' equals: 200 is: [ s size ].
    t check: 'BigComma content head' that: [ (s at: 1) = $a and: [ (s at: 2) = $b ] ].
    t check: 'BigComma content tail' that: [ (s at: 200 - 1) = $a and: [ (s at: 200) = $b ] ].

    "--- recompile stability: run the giant methods again (tier/IC paths) ---"
    t check: 'BigLit second run' equals: 700 is: [ BigLit new build size ].
    t check: 'BigTemps second run' equals: 33525 is: [ BigTemps new calc ].

    ^t report
]
